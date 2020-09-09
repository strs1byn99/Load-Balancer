# Assignment 3

This assignment is to implement a multi-threaded load balancer, which will distribute connections over a set of servers. Queue struct I used in this program is from GeeksforGeeks. Source: https://www.geeksforgeeks.org/queue-set-1introduction-and-array-implementation/

**How to Run:**

On the server side, multiple servers can be on by replacing 8080 with a different port number: 

```bash
./httpserver 8080 -L log_file
```

Open a new terminal to run the following command:

-N is followed by the number of threads, -R will determine the number of requests being processed between health check probes. 

```bash
./loadbalancer -N 7 1234 8080 -R 3 8081
```

On the client side: 

```bash
curl http://localhost:1234/filename & curl -T filename.txt http://localhost:1234/filename & curl -I http://localhost:1234/filename
```

where

```bash
-T, --upload-file <file>
-I, --head
-w, --write-out <format>

curl -s http://localhost:8080/FILENAME
curl -s -T FILENAME http://localhost:8080/FILENAME
curl -s -I http://localhost:8080/FILENAME
```

