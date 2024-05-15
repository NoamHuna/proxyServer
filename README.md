# Proxy Server with Thread Pool

This project implements a simple proxy server in C using a thread pool for handling multiple client requests concurrently.

## Description

The proxy server intercepts client requests, forwards them to the destination server, and relays the responses back to the clients. It utilizes a thread pool to manage multiple client connections efficiently.

## Features

- Concurrent handling of multiple client connections using a thread pool.
- Basic error handling and response generation for various HTTP status codes.
- Filter for blocking access to specific hosts. (example for filter file added)

