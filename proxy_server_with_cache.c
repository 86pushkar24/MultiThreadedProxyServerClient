/*
 * =====================================================================================
 * Multi-Threaded Proxy Server with LRU Cache Implementation
 * =====================================================================================
 *
 * This implementation creates a forward proxy server that:
 * 1. Accepts client HTTP requests
 * 2. Forwards them to remote servers
 * 3. Caches responses using LRU (Least Recently Used) eviction
 * 4. Handles multiple clients concurrently using pthreads
 *
 * Key Concepts Demonstrated:
 * - Socket programming (TCP/IP networking)
 * - HTTP request/response parsing and forwarding
 * - Multi-threading with pthread library
 * - Synchronization using semaphores and mutexes
 * - Memory management and dynamic allocation
 * - Cache implementation with LRU eviction policy
 *
 * Critical Design Questions to Consider:
 * - How does this handle HTTPS requests? (Spoiler: it doesn't)
 * - What happens if the cache grows too large?
 * - How would you add authentication or access control?
 * - Could this be extended to handle HTTP/2 or WebSockets?
 */

#include "proxy_parse.h" // Custom HTTP request parsing library
#include <stdio.h>		 // Standard I/O functions (printf, etc.)
#include <stdlib.h>		 // Memory allocation (malloc, free)
#include <string.h>		 // String manipulation functions
#include <sys/types.h>	 // System data types
#include <sys/socket.h>	 // Socket programming functions
#include <netinet/in.h>	 // Internet address structures
#include <netdb.h>		 // Network database functions (gethostbyname)
#include <arpa/inet.h>	 // IP address conversion functions
#include <unistd.h>		 // POSIX functions (close, read, write)
#include <fcntl.h>		 // File control options
#include <time.h>		 // Time functions for LRU cache timestamps
#include <sys/wait.h>	 // Wait functions for process management
#include <errno.h>		 // Error number definitions
#include <pthread.h>	 // POSIX threads for concurrent client handling
#include <semaphore.h>	 // Semaphore synchronization primitives

/*
 * =====================================================================================
 * CONFIGURATION CONSTANTS
 * =====================================================================================
 * These constants define the operational limits of our proxy server.
 *
 * Design Questions:
 * - Why 4KB for MAX_BYTES? What HTTP scenarios might exceed this?
 * - Is 400 concurrent clients realistic for a production proxy?
 * - How would you make these configurable at runtime?
 */

#define MAX_BYTES 4096					// Maximum size for HTTP request/response buffers
										// Note: Large files or chunked responses may exceed this
#define MAX_CLIENTS 400					// Maximum concurrent client connections
										// Limited by system file descriptor limits and memory
#define MAX_SIZE 200 * (1 << 20)		// Total cache size: 200MB
										// Critical for memory management in production
#define MAX_ELEMENT_SIZE 10 * (1 << 20) // Maximum size per cached response: 10MB
										// Prevents single large responses from dominating cache

/*
 * =====================================================================================
 * CACHE DATA STRUCTURE
 * =====================================================================================
 * Implements a singly-linked list for LRU (Least Recently Used) cache.
 * Each cache element stores a complete HTTP response with metadata.
 *
 * Design Trade-offs:
 * - Linked list allows dynamic sizing but O(n) search time
 * - Hash table would give O(1) lookup but requires more complex memory management
 * - LRU policy balances cache hit rate with implementation simplicity
 */

typedef struct cache_element cache_element;

struct cache_element
{
	char *data;			   // Complete HTTP response (headers + body)
						   // Think: Should we separate headers from body?
	int len;			   // Length of response data in bytes
						   // Critical for proper memory management
	char *url;			   // Original HTTP request URL (cache key)
						   // Question: How do we handle URL variations (http vs https, different params)?
	time_t lru_time_track; // Unix timestamp of last access
						   // Used for LRU eviction - newer timestamp = more recently used
	cache_element *next;   // Pointer to next element in linked list
						   // Simple but requires O(n) traversal for operations
};

/*
 * =====================================================================================
 * FUNCTION DECLARATIONS AND GLOBAL VARIABLES
 * =====================================================================================
 * These declarations provide the interface for cache operations and maintain
 * global state for the proxy server.
 */

// Cache Management Functions
cache_element *find(char *url);							// Search cache for URL, update LRU timestamp
int add_cache_element(char *data, int size, char *url); // Add new response to cache
void remove_cache_element();							// Remove oldest (LRU) element from cache

/*
 * =====================================================================================
 * GLOBAL STATE VARIABLES
 * =====================================================================================
 * These variables maintain the proxy server's state across all threads.
 *
 * Thread Safety Considerations:
 * - port_number, proxy_socketId: Read-only after initialization
 * - tid[]: Each thread writes to different index, generally safe
 * - seamaphore: Thread-safe POSIX semaphore
 * - lock: Protects cache operations from race conditions
 * - head, cache_size: Must be protected by lock during modifications
 */

int port_number = 8080;		// Default listening port - configurable via command line
int proxy_socketId;			// Main server socket descriptor for accepting connections
pthread_t tid[MAX_CLIENTS]; // Thread identifiers for each client handler
							// Question: What happens if we exceed MAX_CLIENTS?

sem_t seamaphore; // Semaphore to limit concurrent connections
				  // Prevents resource exhaustion under high load
				  // Think: Why use semaphore instead of just checking thread count?

pthread_mutex_t lock; // Mutex for thread-safe cache operations
					  // Critical section protection for shared cache data structure
					  // Design Question: Could we use read-write locks for better performance?

cache_element *head; // Head pointer of cache linked list
					 // Global state: requires synchronization
int cache_size;		 // Current total size of cached data in bytes
					 // Used for eviction decisions when approaching MAX_SIZE

/*
 * =====================================================================================
 * ERROR RESPONSE GENERATOR
 * =====================================================================================
 * Generates standardized HTTP error responses for various failure scenarios.
 *
 * HTTP Status Codes Reference:
 * - 400: Bad Request (malformed request syntax)
 * - 403: Forbidden (server understands but refuses to authorize)
 * - 404: Not Found (resource doesn't exist)
 * - 500: Internal Server Error (server encountered unexpected condition)
 * - 501: Not Implemented (server doesn't support requested functionality)
 * - 505: HTTP Version Not Supported
 *
 * Design Questions:
 * - Should we log these errors to a file?
 * - How would you add custom error pages?
 * - Should different error types have different response formats?
 */

int sendErrorMessage(int socket, int status_code)
{
	char str[1024];		  // Buffer for complete HTTP error response
	char currentTime[50]; // RFC-compliant timestamp string
	time_t now = time(0); // Current Unix timestamp

	// Convert timestamp to GMT format for HTTP Date header
	// GMT is required by HTTP/1.1 specification (RFC 7231)
	struct tm data = *gmtime(&now);
	strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", &data);

	switch (status_code)
	{
	case 400:
		// Bad Request: Client sent malformed or invalid request
		// Common causes: Invalid HTTP syntax, missing required headers
		snprintf(str, sizeof(str), "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Rqeuest</H1>\n</BODY></HTML>", currentTime);
		printf("400 Bad Request\n");
		send(socket, str, strlen(str), 0);
		break;

	case 403:
		// Forbidden: Server understood request but refuses to authorize it
		// Could be used for access control, content filtering, etc.
		snprintf(str, sizeof(str), "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", currentTime);
		printf("403 Forbidden\n");
		send(socket, str, strlen(str), 0);
		break;

	case 404:
		// Not Found: Requested resource doesn't exist
		// In proxy context, this might indicate upstream server issues
		snprintf(str, sizeof(str), "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
		printf("404 Not Found\n");
		send(socket, str, strlen(str), 0);
		break;

	case 500:
		// Internal Server Error: Something went wrong in our proxy
		// Should be logged for debugging production issues
		snprintf(str, sizeof(str), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
		// printf("500 Internal Server Error\n");
		send(socket, str, strlen(str), 0);
		break;

	case 501:
		// Not Implemented: Request method not supported
		// Our proxy only supports GET - all others should return 501
		snprintf(str, sizeof(str), "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
		printf("501 Not Implemented\n");
		send(socket, str, strlen(str), 0);
		break;

	case 505:
		// HTTP Version Not Supported
		// We only support HTTP/1.0 and HTTP/1.1
		snprintf(str, sizeof(str), "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
		printf("505 HTTP Version Not Supported\n");
		send(socket, str, strlen(str), 0);
		break;

	default:
		// Unknown error code - should this be logged as a bug?
		return -1;
	}
	return 1; // Return success indicator
}

/*
 * =====================================================================================
 * REMOTE SERVER CONNECTION ESTABLISHMENT
 * =====================================================================================
 * Establishes a TCP connection to the target server specified in the HTTP request.
 * This is the core networking function that enables proxy forwarding.
 *
 * Key Networking Concepts:
 * - DNS resolution (hostname to IP address conversion)
 * - TCP socket creation and connection establishment
 * - Network byte order conversion (htons for port numbers)
 *
 * Critical Design Questions:
 * - How would you add connection pooling to reuse existing connections?
 * - Should we set socket timeouts to prevent hanging connections?
 * - How would you implement connection retry logic?
 * - What about IPv6 support?
 */

int connectRemoteServer(char *host_addr, int port_num)
{
	// Step 1: Create a TCP socket for communication with remote server
	// AF_INET = IPv4, SOCK_STREAM = TCP, 0 = default protocol
	int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);

	if (remoteSocket < 0)
	{
		printf("Error in Creating Socket.\n");
		return -1; // Return error indicator
	}

	// Step 2: Resolve hostname to IP address using DNS
	// gethostbyname() performs DNS lookup - could block for several seconds
	// Modern alternative: getaddrinfo() with better IPv6 support
	struct hostent *host = gethostbyname(host_addr);
	if (host == NULL)
	{
		fprintf(stderr, "No such host exists.\n");
		return -1; // DNS resolution failed
	}

	// Step 3: Set up server address structure for connection
	// This structure tells the kernel where to connect
	struct sockaddr_in server_addr;

	bzero((char *)&server_addr, sizeof(server_addr)); // Clear structure
	server_addr.sin_family = AF_INET;				  // IPv4 address family
	server_addr.sin_port = htons(port_num);			  // Convert port to network byte order

	// Copy IP address from DNS result to address structure
	// bcopy is deprecated - memcpy would be more modern
	bcopy((char *)host->h_addr, (char *)&server_addr.sin_addr.s_addr, host->h_length);

	// Step 4: Establish TCP connection to remote server
	// This performs the TCP three-way handshake
	// Can block for several seconds if server is slow or unreachable
	if (connect(remoteSocket, (struct sockaddr *)&server_addr, (socklen_t)sizeof(server_addr)) < 0)
	{
		fprintf(stderr, "Error in connecting !\n");
		return -1; // Connection failed
	}

	// Connection successful - return socket descriptor
	// Think: Should we set socket options like SO_KEEPALIVE here?
	return remoteSocket;
}

/*
 * =====================================================================================
 * HTTP REQUEST HANDLER AND RESPONSE FORWARDER
 * =====================================================================================
 * This function handles the core proxy logic: reformatting the client's HTTP request,
 * forwarding it to the remote server, receiving the response, and caching it.
 *
 * HTTP Proxy Process Flow:
 * 1. Reconstruct HTTP request with proper headers
 * 2. Connect to remote server
 * 3. Send request to remote server
 * 4. Receive response from remote server
 * 5. Forward response to client
 * 6. Cache response for future requests
 *
 * Key Design Decisions:
 * - Why do we force "Connection: close"? (Prevents HTTP keep-alive complications)
 * - Should we support HTTP/1.1 chunked encoding?
 * - How do we handle very large responses that exceed MAX_BYTES?
 * - What about HTTP redirects (301, 302)?
 */

int handle_request(int clientSocket, ParsedRequest *request, char *tempReq)
{
	// Step 1: Reconstruct HTTP request line
	// Format: "GET /path HTTP/1.1\r\n"
	char *buf = (char *)malloc(sizeof(char) * MAX_BYTES);
	strcpy(buf, "GET ");		// Only support GET method
	strcat(buf, request->path); // Add requested path
	strcat(buf, " ");
	strcat(buf, request->version); // Add HTTP version
	strcat(buf, "\r\n");		   // HTTP line terminator

	size_t len = strlen(buf);

	// Step 2: Set critical HTTP headers
	// Force connection close to simplify proxy logic
	// HTTP/1.1 defaults to keep-alive, but that complicates proxy implementation
	if (ParsedHeader_set(request, "Connection", "close") < 0)
	{
		printf("set header key not work\n");
	}

	// Ensure Host header is present (required by HTTP/1.1)
	// Many servers reject requests without proper Host header
	if (ParsedHeader_get(request, "Host") == NULL)
	{
		if (ParsedHeader_set(request, "Host", request->host) < 0)
		{
			printf("Set \"Host\" header key not working\n");
		}
	}

	// Step 3: Append all headers to request buffer
	// This serializes the parsed headers back into HTTP format
	if (ParsedRequest_unparse_headers(request, buf + len, (size_t)MAX_BYTES - len) < 0)
	{
		printf("unparse failed\n");
		// Continue anyway - some servers might accept requests without headers
	}

	// Step 4: Determine target server port
	int server_port = 80; // Default HTTP port
	if (request->port != NULL)
		server_port = atoi(request->port); // Use specific port if provided

	// Step 5: Connect to remote server
	int remoteSocketID = connectRemoteServer(request->host, server_port);

	if (remoteSocketID < 0)
		return -1; // Connection failed

	// Step 6: Send HTTP request to remote server
	int bytes_send = send(remoteSocketID, buf, strlen(buf), 0);

	bzero(buf, MAX_BYTES); // Clear buffer for receiving response

	// Step 7: Receive response from remote server and forward to client
	bytes_send = recv(remoteSocketID, buf, MAX_BYTES - 1, 0);

	// Dynamic buffer for caching complete response
	char *temp_buffer = (char *)malloc(sizeof(char) * MAX_BYTES);
	int temp_buffer_size = MAX_BYTES;
	int temp_buffer_index = 0;

	// Response forwarding loop
	while (bytes_send > 0)
	{
		// Forward response chunk to client immediately
		bytes_send = send(clientSocket, buf, bytes_send, 0);

		// Store response chunk in cache buffer
		for (int i = 0; i < bytes_send / sizeof(char); i++)
		{
			temp_buffer[temp_buffer_index] = buf[i];
			temp_buffer_index++;
		}

		// Expand cache buffer if needed
		// Critical: This could cause memory issues with very large responses
		temp_buffer_size += MAX_BYTES;
		temp_buffer = (char *)realloc(temp_buffer, temp_buffer_size);

		if (bytes_send < 0)
		{
			perror("Error in sending data to client socket.\n");
			break;
		}

		bzero(buf, MAX_BYTES); // Clear buffer for next chunk
		bytes_send = recv(remoteSocketID, buf, MAX_BYTES - 1, 0);
	}

	// Step 8: Cache the complete response
	temp_buffer[temp_buffer_index] = '\0'; // Null-terminate
	free(buf);
	add_cache_element(temp_buffer, strlen(temp_buffer), tempReq);
	printf("Done\n");
	free(temp_buffer);

	close(remoteSocketID); // Close connection to remote server
	return 0;
}

/*
 * =====================================================================================
 * HTTP VERSION VALIDATION
 * =====================================================================================
 * Validates whether the HTTP version in the request is supported by our proxy.
 *
 * Supported Versions:
 * - HTTP/1.0: Original HTTP with simple request/response
 * - HTTP/1.1: Adds persistent connections, chunked encoding, host headers
 *
 * Unsupported Versions:
 * - HTTP/0.9: Too primitive, rarely used
 * - HTTP/2.0: Binary protocol, requires different parsing
 * - HTTP/3.0: QUIC-based, completely different transport
 *
 * Critical Questions:
 * - Should we treat HTTP/1.0 and HTTP/1.1 differently?
 * - How would you add HTTP/2 support?
 * - Should we downgrade HTTP/1.1 to HTTP/1.0 for simpler proxy logic?
 */

int checkHTTPversion(char *msg)
{
	int version = -1; // Default to invalid version

	if (strncmp(msg, "HTTP/1.1", 8) == 0)
	{
		version = 1; // HTTP/1.1 supported
	}
	else if (strncmp(msg, "HTTP/1.0", 8) == 0)
	{
		version = 1; // HTTP/1.0 supported (treating same as 1.1)
	}
	else
	{
		version = -1; // Unsupported version
	}

	return version;
}

/*
 * =====================================================================================
 * CLIENT THREAD HANDLER - THE HEART OF THE PROXY
 * =====================================================================================
 * This function runs in a separate thread for each client connection.
 * It implements the complete proxy workflow: receive request, check cache,
 * forward to server if needed, and return response.
 *
 * Threading Architecture:
 * - Each client gets dedicated thread (avoid blocking other clients)
 * - Semaphore controls max concurrent threads (prevent resource exhaustion)
 * - Mutex protects shared cache data structure
 *
 * HTTP Processing Pipeline:
 * 1. Receive complete HTTP request from client
 * 2. Parse and validate HTTP request
 * 3. Check cache for existing response
 * 4. If cache miss: forward request to origin server
 * 5. Send response back to client
 * 6. Clean up resources and signal thread completion
 *
 * Critical Design Questions:
 * - What happens if a client sends malformed HTTP?
 * - How do we handle slow clients that don't send complete requests?
 * - Should we support HTTP pipelining (multiple requests per connection)?
 * - How would you implement request/response logging?
 */

void *thread_fn(void *socketNew)
{
	// Step 1: Acquire semaphore slot to prevent server overload
	// This blocks if we already have MAX_CLIENTS active threads
	sem_wait(&seamaphore);
	int p;
	sem_getvalue(&seamaphore, &p);
	printf("semaphore value:%d\n", p); // Debug: show available slots

	// Step 2: Extract client socket from thread argument
	int *t = (int *)(socketNew);
	int socket = *t;			// Client socket descriptor
	int bytes_send_client, len; // Byte counters for network operations

	// Step 3: Allocate buffer for receiving HTTP request
	char *buffer = (char *)calloc(MAX_BYTES, sizeof(char)); // Zero-initialized buffer

	bzero(buffer, MAX_BYTES);								// Extra safety - clear buffer
	bytes_send_client = recv(socket, buffer, MAX_BYTES, 0); // Receive initial data

	// Step 4: Receive complete HTTP request
	// HTTP requests end with "\r\n\r\n" - we must receive the complete request
	while (bytes_send_client > 0)
	{
		len = strlen(buffer);
		// Keep receiving until we find the HTTP request terminator
		if (strstr(buffer, "\r\n\r\n") == NULL)
		{
			// Request incomplete - receive more data
			bytes_send_client = recv(socket, buffer + len, MAX_BYTES - len, 0);
		}
		else
		{
			break; // Complete HTTP request received
		}
	}

	// Debug output (commented) - useful for troubleshooting
	// printf("--------------------------------------------\n");
	// printf("%s\n",buffer);
	// printf("----------------------%d----------------------\n",strlen(buffer));

	// Step 5: Create copy of request for cache key
	char *tempReq = (char *)malloc(strlen(buffer) * sizeof(char) + 1);
	// Both tempReq and buffer store the complete HTTP request
	for (int i = 0; i < strlen(buffer); i++)
	{
		tempReq[i] = buffer[i];
	}

	// Step 6: Check if response exists in cache
	struct cache_element *temp = find(tempReq);

	if (temp != NULL)
	{
		// CACHE HIT: Send cached response directly to client
		printf("Data retrived from the Cache\n\n");

		int size = temp->len / sizeof(char);
		int pos = 0;
		char response[MAX_BYTES];

		// Send cached response in chunks
		while (pos < size)
		{
			bzero(response, MAX_BYTES);
			for (int i = 0; i < MAX_BYTES; i++)
			{
				response[i] = temp->data[pos]; // Copy cached data to response buffer
				pos++;
			}
			send(socket, response, MAX_BYTES, 0); // Send chunk to client
		}
		printf("%s\n\n", response);
	}

	else if (bytes_send_client > 0)
	{
		// CACHE MISS: Process request and forward to origin server
		len = strlen(buffer);

		// Step 7: Parse HTTP request using our custom parser
		ParsedRequest *request = ParsedRequest_create();

		// ParsedRequest_parse returns 0 on success, -1 on failure
		if (ParsedRequest_parse(request, buffer, len) < 0)
		{
			printf("Parsing failed\n");
			// Should we send 400 Bad Request here?
		}
		else
		{
			bzero(buffer, MAX_BYTES); // Clear buffer for reuse

			// Step 8: Validate request method and version
			if (!strcmp(request->method, "GET"))
			{
				// Only support GET method (most common for web browsing)
				if (request->host && request->path && (checkHTTPversion(request->version) == 1))
				{
					// Valid GET request - forward to origin server
					bytes_send_client = handle_request(socket, request, tempReq);
					if (bytes_send_client == -1)
					{
						sendErrorMessage(socket, 500); // Forward failed
					}
				}
				else
				{
					sendErrorMessage(socket, 500); // Missing required fields
				}
			}
			else
			{
				// Unsupported HTTP method (POST, PUT, DELETE, etc.)
				printf("This code doesn't support any method other than GET\n");
				sendErrorMessage(socket, 501); // Not Implemented
			}
		}

		// Step 9: Clean up parsed request
		ParsedRequest_destroy(request);
	}

	else if (bytes_send_client < 0)
	{
		// Network error while receiving from client
		perror("Error in receiving from client.\n");
	}
	else if (bytes_send_client == 0)
	{
		// Client closed connection gracefully
		printf("Client disconnected!\n");
	}

	// Step 10: Clean up and release resources
	shutdown(socket, SHUT_RDWR); // Graceful socket shutdown
	close(socket);				 // Close client connection
	free(buffer);				 // Free request buffer
	sem_post(&seamaphore);		 // Release semaphore slot for new clients

	sem_getvalue(&seamaphore, &p);
	printf("Semaphore post value:%d\n", p); // Debug: show available slots
	free(tempReq);							// Free cache key copy
	return NULL;							// Thread terminates
}

/*
 * =====================================================================================
 * MAIN FUNCTION - PROXY SERVER INITIALIZATION AND CONTROL LOOP
 * =====================================================================================
 * Sets up the proxy server infrastructure and runs the main accept loop.
 * This function handles:
 * - Command line argument parsing
 * - Server socket creation and binding
 * - Thread synchronization primitive initialization
 * - Infinite loop accepting client connections
 * - Thread creation for each client
 *
 * Server Architecture:
 * - One main thread handles accept() loop
 * - One worker thread per client connection
 * - Semaphore limits concurrent connections
 * - Shared cache protected by mutex
 *
 * Design Questions:
 * - What happens if we run out of thread IDs (i exceeds MAX_CLIENTS)?
 * - Should we implement graceful shutdown (signal handling)?
 * - How would you add configuration file support?
 * - Should we implement connection pooling for efficiency?
 */

int main(int argc, char *argv[])
{
	// Variables for client connection handling
	int client_socketId, client_len;			 // Store client socket and address length
	struct sockaddr_in server_addr, client_addr; // Network address structures

	// Step 1: Initialize synchronization primitives
	sem_init(&seamaphore, 0, MAX_CLIENTS); // Semaphore to limit concurrent threads
	pthread_mutex_init(&lock, NULL);	   // Mutex for thread-safe cache operations

	// Step 2: Parse command line arguments
	if (argc == 2) // Expect: ./proxy <port>
	{
		port_number = atoi(argv[1]); // Convert string to integer
	}
	else
	{
		printf("Usage: %s <port>\n", argv[0]);
		printf("Example: %s 8080\n", argv[0]);
		exit(1);
	}

	printf("Setting Proxy Server Port : %d\n", port_number);

	// Step 3: Create main server socket
	// AF_INET = IPv4, SOCK_STREAM = TCP, 0 = default protocol
	proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);

	if (proxy_socketId < 0)
	{
		perror("Failed to create socket.\n");
		exit(1);
	}

	// Step 4: Configure socket options
	// SO_REUSEADDR allows immediate reuse of port after server restart
	// Without this, you'd get "Address already in use" errors
	int reuse = 1;
	if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) < 0)
		perror("setsockopt(SO_REUSEADDR) failed\n");

	// Step 5: Configure server address structure
	bzero((char *)&server_addr, sizeof(server_addr)); // Clear structure
	server_addr.sin_family = AF_INET;				  // IPv4
	server_addr.sin_port = htons(port_number);		  // Convert port to network byte order
	server_addr.sin_addr.s_addr = INADDR_ANY;		  // Accept connections on any interface

	// Step 6: Bind socket to address and port
	if (bind(proxy_socketId, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
	{
		perror("Port is not free\n");
		exit(1);
	}
	printf("Binding on port: %d\n", port_number);

	// Step 7: Start listening for connections
	// MAX_CLIENTS defines the connection backlog queue size
	int listen_status = listen(proxy_socketId, MAX_CLIENTS);

	if (listen_status < 0)
	{
		perror("Error while Listening !\n");
		exit(1);
	}

	// Step 8: Initialize client connection tracking
	int i = 0;							 // Thread counter
	int Connected_socketId[MAX_CLIENTS]; // Array to store client socket descriptors
										 // Critical: What if i exceeds MAX_CLIENTS?

	// Step 9: Main server loop - accept connections infinitely
	printf("Proxy server started successfully. Waiting for connections...\n");
	while (1)
	{
		// Clear client address structure
		bzero((char *)&client_addr, sizeof(client_addr));
		client_len = sizeof(client_addr);

		// Accept incoming connection (blocking call)
		client_socketId = accept(proxy_socketId, (struct sockaddr *)&client_addr, (socklen_t *)&client_len);
		if (client_socketId < 0)
		{
			fprintf(stderr, "Error in Accepting connection !\n");
			exit(1);
		}
		else
		{
			Connected_socketId[i] = client_socketId; // Store client socket
		}

		// Step 10: Extract client connection information
		struct sockaddr_in *client_pt = (struct sockaddr_in *)&client_addr; // Cast to sockaddr_in
		struct in_addr ip_addr = client_pt->sin_addr;						// Extract IP address
		char str[INET_ADDRSTRLEN];											// Buffer for IP string representation
		inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN);					// Convert IP to string
		printf("Client is connected with port number: %d and ip address: %s \n", ntohs(client_addr.sin_port), str);

		// Step 11: Create dedicated thread for this client
		// Each client gets its own thread to prevent blocking
		pthread_create(&tid[i], NULL, thread_fn, (void *)&Connected_socketId[i]);
		i++; // Increment thread counter
			 // BUG: What happens when i reaches MAX_CLIENTS?
	}

	// This code is never reached due to infinite loop above
	// Should we add signal handling for graceful shutdown?
	close(proxy_socketId);
	return 0;
}

/*
 * =====================================================================================
 * CACHE LOOKUP FUNCTION - LRU CACHE SEARCH WITH TIME UPDATE
 * =====================================================================================
 * Searches the cache for a specific URL and returns the cached response if found.
 * Implements the "Recent Use" part of LRU by updating the timestamp on cache hits.
 *
 * Cache Search Algorithm:
 * 1. Acquire mutex lock for thread-safe access to shared cache
 * 2. Linear search through linked list (O(n) complexity)
 * 3. If found: update LRU timestamp to mark as recently used
 * 4. Return pointer to cache element or NULL if not found
 *
 * Key Design Decisions:
 * - Uses string comparison (!strcmp) for URL matching
 * - Updates LRU timestamp on every access (not just writes)
 * - Thread-safe through mutex locking
 * - Linear search trade-off: simple but O(n) performance
 *
 * Critical Design Questions:
 * - Why linear search instead of hash table for O(1) lookup?
 * - Should URL comparison be case-sensitive? What about query parameters?
 * - How would you handle URL normalization (trailing slashes, default ports)?
 * - Could this be optimized with a hash map + doubly-linked list (like Redis)?
 * - What happens if two threads try to access the same cache element?
 * - Should we implement read-write locks for better concurrent performance?
 */

cache_element *find(char *url)
{
	// Initialize search pointer - will traverse the entire cache linked list
	cache_element *site = NULL;

	// Step 1: Acquire exclusive lock for thread-safe cache access
	// Critical section: protects shared cache data structure from race conditions
	// Multiple threads might try to read/write cache simultaneously
	int temp_lock_val = pthread_mutex_lock(&lock);
	printf("Cache Search Lock Acquired %d\n", temp_lock_val); // Debug output

	// Step 2: Check if cache contains any elements
	if (head != NULL)
	{
		// Cache is not empty - begin linear search from head
		site = head;

		// Step 3: Traverse linked list to find matching URL
		// Time Complexity: O(n) where n = number of cached responses
		// Space Complexity: O(1) - only using pointer traversal
		while (site != NULL)
		{
			// Step 4: Compare current cache element URL with search target
			// strcmp returns 0 for exact string match
			// Note: This is case-sensitive and requires exact URL match
			if (!strcmp(site->url, url))
			{
				// CACHE HIT: Found matching URL in cache
				printf("LRU Time Track Before: %ld", site->lru_time_track);
				printf("\nURL found in cache\n");

				// Step 5: Update LRU timestamp - mark as recently accessed
				// This is crucial for LRU eviction policy
				// Most recently used items have highest timestamps
				site->lru_time_track = time(NULL); // Current Unix timestamp
				printf("LRU Time Track After: %ld", site->lru_time_track);

				break; // Exit search loop - we found our target
			}

			// Move to next element in linked list
			site = site->next;
		}

		// If we exit the loop with site == NULL, URL was not found
	}
	else
	{
		// Cache is empty - no elements to search
		printf("\nCache is empty - URL not found\n");
	}

	// Step 6: Release mutex lock and return result
	temp_lock_val = pthread_mutex_unlock(&lock);
	printf("Cache Search Lock Released %d\n", temp_lock_val);

	// Return value: pointer to cache element if found, NULL if not found
	// Caller should check for NULL before accessing returned pointer
	return site;
}

/*
 * =====================================================================================
 * CACHE EVICTION FUNCTION - LRU (LEAST RECENTLY USED) ELEMENT REMOVAL
 * =====================================================================================
 * Removes the least recently used element from the cache to free up space.
 * This is the "Least Recently Used" part of the LRU cache eviction policy.
 *
 * LRU Eviction Algorithm:
 * 1. Find element with smallest (oldest) lru_time_track timestamp
 * 2. Remove it from the linked list (update pointers)
 * 3. Free all associated memory (data, url, struct)
 * 4. Update global cache_size counter
 *
 * Key Implementation Details:
 * - Uses three pointers for safe linked list removal: prev, current, target
 * - Handles special case where head element is the LRU element
 * - Thread-safe through mutex locking
 * - Updates global cache_size for space management
 *
 * Critical Design Questions:
 * - Why not use a doubly-linked list for easier removal?
 * - Should we implement a heap or priority queue for faster LRU identification?
 * - What happens if multiple elements have the same timestamp?
 * - How would you add cache access statistics (hit/miss ratios)?
 * - Should there be a minimum cache size to prevent complete eviction?
 * - Could we implement batch eviction for better performance?
 * - What about implementing different eviction policies (LFU, FIFO, Random)?
 */

void remove_cache_element()
{
	// Pointer management for safe linked list traversal and removal
	cache_element *p;	 // Previous pointer - tracks element before target
	cache_element *q;	 // Current pointer - used for list traversal
	cache_element *temp; // Target pointer - points to element to be removed

	// Step 1: Acquire exclusive lock for thread-safe cache modification
	// Critical section: prevents race conditions during cache structure changes
	int temp_lock_val = pthread_mutex_lock(&lock);
	printf("Cache Eviction Lock Acquired %d\n", temp_lock_val);

	// Step 2: Verify cache is not empty before attempting removal
	if (head != NULL)
	{
		// Cache contains at least one element - proceed with LRU search

		// Step 3: Initialize pointers for LRU element identification
		// All three pointers start at head - we'll update them during traversal
		for (q = head, p = head, temp = head; q->next != NULL; q = q->next)
		{
			// Step 4: Compare timestamps to find least recently used element
			// Lower timestamp = older access time = higher priority for eviction
			if (((q->next)->lru_time_track) < (temp->lru_time_track))
			{
				temp = q->next; // Update target to older element
				p = q;			// Update previous pointer for safe removal
			}
		}

		// Step 5: Remove target element from linked list
		// Handle two cases: target is head vs. target is middle/end
		if (temp == head)
		{
			// Special case: LRU element is the head of the list
			// Update head pointer to second element (or NULL if only one element)
			head = head->next;
		}
		else
		{
			// General case: LRU element is not the head
			// Bridge the gap: previous element points to element after target
			p->next = temp->next;
		}

		// Step 6: Update global cache size counter
		// Subtract: response data size + cache element metadata + URL string + null terminator
		// This calculation must match the addition in add_cache_element()
		cache_size = cache_size - (temp->len) - sizeof(cache_element) - strlen(temp->url) - 1;

		// Step 7: Free all memory associated with removed element
		// Order matters: free data first, then URL, then struct
		// This prevents memory leaks and dangling pointers
		free(temp->data); // Free HTTP response data
		free(temp->url);  // Free URL string (cache key)
		free(temp);		  // Free cache element structure

		printf("Cache element evicted - LRU policy applied\n");
	}
	else
	{
		// Edge case: attempted to remove from empty cache
		// This shouldn't happen in normal operation but provides safety
		printf("Warning: Attempted to remove from empty cache\n");
	}

	// Step 8: Release mutex lock
	temp_lock_val = pthread_mutex_unlock(&lock);
	printf("Cache Eviction Lock Released %d\n", temp_lock_val);
}

/*
 * =====================================================================================
 * CACHE INSERTION FUNCTION - ADD NEW HTTP RESPONSE TO LRU CACHE
 * =====================================================================================
 * Adds a new HTTP response to the cache with intelligent space management.
 * Implements cache size limits and automatic eviction when space is needed.
 *
 * Cache Insertion Algorithm:
 * 1. Calculate total memory footprint of new element
 * 2. Validate element size against maximum allowed size
 * 3. Evict old elements if necessary to make space
 * 4. Allocate memory and create new cache element
 * 5. Insert at head of linked list (most recently used position)
 * 6. Update global cache size counter
 *
 * Memory Management Strategy:
 * - Each cache element contains: HTTP response data + URL key + metadata
 * - Total cache size limited by MAX_SIZE (200MB default)
 * - Individual elements limited by MAX_ELEMENT_SIZE (10MB default)
 * - Automatic eviction prevents unbounded memory growth
 *
 * Critical Design Questions:
 * - Why insert at head instead of tail of linked list?
 * - Should we compress cached responses to save memory?
 * - How would you implement cache persistence (save to disk)?
 * - What about cache expiration based on HTTP headers (Cache-Control, Expires)?
 * - Should we cache partial responses or only complete ones?
 * - How would you implement cache statistics and monitoring?
 * - What about implementing cache warming strategies?
 */

int add_cache_element(char *data, int size, char *url)
{
	// Step 1: Acquire exclusive lock for thread-safe cache modification
	// Critical section: prevents race conditions during cache structure changes
	int temp_lock_val = pthread_mutex_lock(&lock);
	printf("Cache Addition Lock Acquired %d\n", temp_lock_val);

	// Step 2: Calculate total memory footprint of new cache element
	// Memory components: response data + URL string + null terminator + struct metadata
	// This calculation is crucial for accurate cache size management
	int element_size = size + 1 + strlen(url) + sizeof(cache_element);

	// Step 3: Validate element size against maximum allowed size
	// Prevents single large responses from dominating or breaking the cache
	// Example: A 50MB video response would be rejected if MAX_ELEMENT_SIZE is 10MB
	if (element_size > MAX_ELEMENT_SIZE)
	{
		// Element too large for cache - reject insertion
		temp_lock_val = pthread_mutex_unlock(&lock);
		printf("Cache Addition Lock Released %d\n", temp_lock_val);
		printf("Warning: Element size (%d bytes) exceeds maximum (%d bytes) - not cached\n",
			   element_size, MAX_ELEMENT_SIZE);

		// Return 0 to indicate cache insertion failed
		// Caller should handle this gracefully (request still works, just not cached)
		return 0;
	}
	else
	{
		// Element size acceptable - proceed with insertion

		// Step 4: Ensure sufficient cache space through eviction
		// Keep removing LRU elements until we have enough space for new element
		// This loop implements the cache size limit enforcement
		while (cache_size + element_size > MAX_SIZE)
		{
			// Evict least recently used element to make space
			// Note: remove_cache_element() handles empty cache gracefully
			remove_cache_element();
			printf("Evicted LRU element to make space for new cache entry\n");
		}

		// Step 5: Allocate memory for new cache element and its data
		// Three separate allocations: struct, response data, URL string
		cache_element *element = (cache_element *)malloc(sizeof(cache_element));

		// Allocate memory for HTTP response data (size + 1 for null terminator)
		element->data = (char *)malloc(size + 1);
		strcpy(element->data, data); // Copy complete HTTP response

		// Allocate memory for URL string (cache key)
		// Size calculation: URL length + 1 for null terminator
		element->url = (char *)malloc(1 + (strlen(url) * sizeof(char)));
		strcpy(element->url, url); // Copy URL for cache key lookup

		// Step 6: Initialize cache element metadata
		element->lru_time_track = time(NULL); // Current timestamp - marks as most recently used
		element->len = size;				  // Store response size for memory management

		// Step 7: Insert new element at head of linked list
		// Head position = most recently used position in LRU ordering
		// This maintains LRU invariant: head = newest, tail = oldest
		element->next = head; // New element points to current head
		head = element;		  // Update head to point to new element

		// Step 8: Update global cache size counter
		// Add total memory footprint to running total
		cache_size += element_size;

		printf("Cache element added successfully - Size: %d bytes, Total cache: %d bytes\n",
			   element_size, cache_size);

		// Step 9: Release mutex lock and return success
		temp_lock_val = pthread_mutex_unlock(&lock);
		printf("Cache Addition Lock Released %d\n", temp_lock_val);

		// Return 1 to indicate successful cache insertion
		return 1;
	}

	// This return should never be reached due to if-else structure above
	// Added for completeness and to satisfy compiler warnings
	return 0;
}
