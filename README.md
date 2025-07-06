# Multi-Threaded HTTP Proxy Server with LRU Cache

A high-performance HTTP proxy server implemented in C that handles multiple concurrent clients using pthreads, features an intelligent LRU cache system for response optimization, and demonstrates advanced socket programming with comprehensive error handling and thread synchronization.

## Table of Contents

- [Features](#features)
- [Architecture Overview](#architecture-overview)
- [File Structure](#file-structure)
- [Build Instructions](#build-instructions)
- [Usage](#usage)
- [Testing](#testing)
- [Configuration](#configuration)
- [Limitations](#limitations)
- [Future Enhancements](#future-enhancements)
- [Contributing](#contributing)
- [License](#license)

## Features

### Core Functionality
- **HTTP Proxy Server**: Forwards client HTTP requests to remote servers and returns responses
- **Multi-threaded Architecture**: Handles up to 400 concurrent client connections using POSIX threads
- **LRU Cache System**: Intelligently caches HTTP responses with Least Recently Used eviction policy
- **Thread-Safe Operations**: Uses semaphores and mutexes for safe concurrent access to shared resources

### Advanced Features
- **Smart Memory Management**: Dynamic cache sizing with configurable limits (200MB total, 10MB per element)
- **HTTP Protocol Support**: Handles HTTP/1.0 and HTTP/1.1 requests with proper header management
- **Error Handling**: Comprehensive HTTP error responses (400, 403, 404, 500, 501, 505)
- **Connection Management**: Automatic connection cleanup and resource management
- **Debug Output**: Detailed logging for cache operations and client connections

### Performance Optimizations
- **Cache Hit Optimization**: Instant response delivery for cached content
- **Connection Limiting**: Semaphore-based connection throttling prevents resource exhaustion
- **Efficient Data Structures**: Linked-list based cache with O(1) insertion and removal
- **Memory Pooling**: Reusable buffer allocation for HTTP request/response handling

## Architecture Overview
![[pics/UML.JPG]](pics/UML.JPG)
### High-Level Flow
```
Client Request → Proxy Server → Cache Check → Origin Server (if cache miss) → Response Caching → Client Response
```

### Threading Model
- **Main Thread**: Accepts incoming client connections and spawns worker threads
- **Worker Threads**: Handle individual client requests, cache operations, and response forwarding
- **Synchronization**: Semaphores limit concurrent connections; mutexes protect shared cache data

### Cache Architecture
- **Data Structure**: Singly-linked list for dynamic sizing
- **Eviction Policy**: LRU (Least Recently Used) based on access timestamps
- **Thread Safety**: Mutex-protected operations for concurrent access
- **Memory Management**: Automatic eviction when approaching size limits

### Request Processing Pipeline
1. **Connection Acceptance**: Main thread accepts client connection
2. **Request Parsing**: Worker thread parses HTTP request using custom parser
3. **Cache Lookup**: Search cache for existing response
4. **Origin Server Communication**: Forward request if cache miss
5. **Response Processing**: Cache response and forward to client
6. **Resource Cleanup**: Close connections and free memory

## File Structure

```
MultiThreadedProxyServerClient/
├── proxy_server_with_cache.c    # Main proxy server implementation with caching
├── proxy_server_without_cache.c # Simplified version without caching
├── proxy_parse.c                # HTTP request parsing library
├── proxy_parse.h                # Header file for parsing functions
├── Makefile                     # Build configuration
├── README.md                    # Project documentation
└── pics/                        # Demo images
    ├── cache.png               # Cache demonstration
    └── UML.JPG                 # Architecture diagram
```

### Key Files Description

**`proxy_server_with_cache.c`** (Main Implementation)
- Multi-threaded proxy server with LRU cache
- Handles HTTP request/response forwarding
- Implements thread-safe cache operations
- Comprehensive error handling and logging

**`proxy_parse.c` & `proxy_parse.h`** (HTTP Parser)
- Custom HTTP request parsing library
- Header manipulation functions
- Request validation and formatting
- Memory management for parsed requests

**`Makefile`** (Build System)
- Compilation configuration for all variants
- Links required libraries (pthread, etc.)
- Provides clean build targets

## Build Instructions

### Prerequisites
- **Operating System**: Linux (POSIX-compliant system)
- **Compiler**: GCC with C99 support
- **Libraries**: pthread, standard C library

### Compilation
```bash
# Clone the repository
git clone https://github.com/86pushkar24/MultiThreadedProxyServerClient.git
cd MultiThreadedProxyServerClient

# Build all variants
make all

# Or build specific versions
make proxy_server_with_cache    # Main cached version
make proxy_server_without_cache # Simple version without cache
```

### Build Targets
```bash
make all           # Build all executables
make clean         # Remove compiled files
make proxy         # Build main proxy with cache
```

## Usage

### Starting the Proxy Server
```bash
# Start proxy server on port 8080
./proxy_server_with_cache 8080

# Start proxy server on custom port
./proxy_server_with_cache 3128
```

### Expected Output
```
Setting Proxy Server Port : 8080
Binding on port: 8080
Proxy server started successfully. Waiting for connections...
```

### Client Configuration

**Browser Configuration:**
1. Configure your browser to use `localhost:8080` as HTTP proxy
2. **Important**: Disable browser cache to test proxy caching functionality

**Direct URL Access:**
```
http://localhost:8080/http://example.com
```

### Command Line Usage
```bash
# Test with curl
curl -x localhost:8080 http://example.com

# Test with specific headers
curl -x localhost:8080 -H "User-Agent: TestClient" http://httpbin.org/get
```

## Testing

### Cache Functionality Testing
```bash
# First request (cache miss)
curl -x localhost:8080 http://httpbin.org/get
# Output: "URL not found" - cache miss

# Second request (cache hit)
curl -x localhost:8080 http://httpbin.org/get
# Output: "Data retrieved from the Cache" - cache hit
```

### Multi-threading Testing
```bash
# Test concurrent connections
for i in {1..10}; do
    curl -x localhost:8080 http://example.com &
done
wait
```

### Error Handling Testing
```bash
# Test invalid requests
curl -x localhost:8080 -X POST http://example.com  # Should return 501
curl -x localhost:8080 http://nonexistent.invalid  # Should return 500
```

### Performance Testing
```bash
# Monitor cache performance
watch -n 1 'curl -s -x localhost:8080 http://example.com | head -n 5'
```

## Configuration

### Compile-time Configuration
Edit `proxy_server_with_cache.c` to modify:

```c
#define MAX_BYTES 4096                 // Request/response buffer size
#define MAX_CLIENTS 400                // Maximum concurrent connections
#define MAX_SIZE 200 * (1 << 20)       // Total cache size (200MB)
#define MAX_ELEMENT_SIZE 10 * (1 << 20) // Max cached response size (10MB)
```

### Runtime Configuration
- **Port**: Specified as command-line argument
- **Cache**: Automatically managed with LRU eviction
- **Connections**: Limited by MAX_CLIENTS semaphore

## Limitations

### Current Limitations
- **HTTP Only**: No HTTPS/SSL support (requires tunneling implementation)
- **GET Method Only**: POST, PUT, DELETE methods return 501 Not Implemented
- **Cache Size**: Fixed maximum sizes may not suit all use cases
- **IPv4 Only**: No IPv6 support in current implementation
- **Buffer Size**: 4KB limit may truncate large responses

### Known Issues
- **Thread Counter**: Potential overflow after MAX_CLIENTS connections
- **Memory Leaks**: Possible leaks under high connection churn
- **Error Recovery**: Limited recovery from network errors

## Future Enhancements

### High Priority
- **HTTPS Support**: Implement CONNECT method for SSL tunneling
- **HTTP/2 Support**: Add binary protocol support
- **Configuration File**: Runtime configuration without recompilation
- **Logging System**: Structured logging with log levels

### Medium Priority
- **Cache Improvements**: 
  - Hash table for O(1) cache lookup
  - Cache persistence across restarts
  - TTL-based cache expiration
- **Protocol Extensions**:
  - WebSocket support
  - HTTP/3 (QUIC) support
  - More HTTP methods (POST, PUT, DELETE)

### Low Priority
- **Performance Optimizations**:
  - Connection pooling
  - Asynchronous I/O
  - Load balancing
- **Security Features**:
  - Access control lists
  - Rate limiting
  - Request filtering

### Development Improvements
- **Testing Framework**: Automated unit and integration tests
- **Documentation**: API documentation and code coverage
- **Monitoring**: Performance metrics and health checks

## Contributing

We welcome contributions! Here's how you can help:

### Getting Started
1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Make your changes with proper comments
4. Add tests for new functionality
5. Commit changes (`git commit -m 'Add amazing feature'`)
6. Push to branch (`git push origin feature/amazing-feature`)
7. Open a Pull Request

### Contribution Guidelines
- Follow existing code style and commenting patterns
- Add comprehensive comments for complex functions
- Test your changes thoroughly
- Update documentation as needed
- Consider performance implications

### Areas for Contribution
- Bug fixes and error handling improvements
- Performance optimizations
- New feature implementations
- Documentation improvements
- Test coverage expansion

## License

This project is available under the MIT License. See the LICENSE file for more details.

---

**Note**: This proxy server is designed for educational purposes and local development. For production use, consider additional security hardening, monitoring, and performance optimizations.

### Demo
![Cache Demo](pics/cache.png)

*Cache hit/miss demonstration showing the difference between first request (cache miss) and subsequent requests (cache hit)*
