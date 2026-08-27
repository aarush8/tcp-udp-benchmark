# Understanding the benchmark from scratch

The project’s basic idea is:

**Send a known amount of data to another program, have that program send it back, check that it came back correctly, and measure the time and computer resources required.**

Everything else in the project exists to make those measurements correct, configurable, repeatable, and explainable.

I’ll build up from the basics. The numerical examples used to teach concepts are labeled as examples; the measured results come from the experiments saved in your repository.

1. **A computer runs instructions and moves data.**

   Three computer components matter most here:

   | Component | What it does | Why it matters to this project |
   |---|---|---|
   | CPU | Executes instructions | Runs our program, copies data, checks results, and performs networking work |
   | RAM, usually called memory | Holds data currently being used | Stores the messages and temporary working areas |
   | Storage, such as an SSD | Keeps files after programs stop | Stores the source code, executable programs, and saved results |

   The **operating system** manages these resources. Your operating system is Linux.

   The central part of an operating system is called the **kernel**. It manages things such as memory, running programs, and network communication.

   Our program does not directly control your network hardware. It asks Linux to perform networking operations.

   A useful picture is:

   ```text
   Our program
       |
       | asks Linux to send or receive data
       v
   Linux kernel
       |
       | handles the underlying networking
       v
   Destination
   ```

   This distinction matters: **we built an application that uses TCP and UDP. We did not implement TCP and UDP themselves.**

2. **The data being moved is made of bytes.**

   A **bit** is a value that can be either `0` or `1`.

   A **byte** is eight bits.

   One byte can represent an integer from 0 through 255. Larger values, text, pictures, and practically everything else are represented using combinations of bytes.

   Our benchmark does not need meaningful text or a real image. It needs a known collection of bytes whose size and contents we control.

   These units appear throughout the project:

   | Unit | Meaning |
   |---|---|
   | 1 byte | 8 bits |
   | 1 KiB | 1,024 bytes |
   | 1 MiB | 1,048,576 bytes |
   | 1 Mbps | 1,000,000 bits per second |
   | 1 Gbps | 1,000,000,000 bits per second |

   The capitalization matters: **bytes and bits are different units**.

   For example, transferring 1,000,000 bytes in one second is:

   ```text
   1,000,000 bytes × 8 bits per byte
       = 8,000,000 bits

   Rate = 8 Mbps
   ```

   That is an arithmetic example, not one of our measured results.

   In our program, **payload** means the application data we want to transfer. It excludes the extra information needed to organize and deliver that data.

   So:

   ```bash
   --payload 1024
   ```

   means “put 1,024 bytes of application data in each logical message.”

3. **C++ is the language we used to describe the program.**

   Source code is text containing instructions written in a programming language.

   Here is a small C++ example:

   ```cpp
   uint32_t payload = 1024;
   ```

   Read it as:

   > Create a variable named `payload` that holds a nonnegative whole number, and give it the value 1024.

   A **variable** is a named place to hold a value.

   A **type** describes what kind of value a variable holds. In this example, `uint32_t` is the type.

   Some types and constructs in your project are:

   | C++ construct | Meaning |
   |---|---|
   | `uint8_t` | An unsigned 8-bit integer; useful for one byte |
   | `uint32_t` | An unsigned 32-bit integer |
   | `uint64_t` | An unsigned 64-bit integer; useful for timestamps and counters |
   | `double` | A number that can have a fractional part |
   | `bool` | Either `true` or `false` |
   | `std::string` | Text |
   | `std::vector<uint8_t>` | A collection of bytes whose size can be chosen while the program runs |
   | `std::array<uint8_t, 24>` | A collection containing exactly 24 bytes |

   **Unsigned** means the integer does not represent negative values.

   The `std::` prefix means the feature comes from the C++ standard library: commonly available building blocks provided with C++.

   A **function** is a named operation. For example:

   ```cpp
   uint64_t now_ns();
   ```

   declares a function named `now_ns` that returns a 64-bit integer.

   In our project, that returned integer represents a clock reading in nanoseconds.

   A **condition** chooses what to do:

   ```cpp
   if (received_bytes == expected_bytes) {
       // Perform an action when these values match.
   }
   ```

   A **loop** repeats an operation:

   ```cpp
   while (received_bytes < expected_bytes) {
       // Keep receiving more data.
   }
   ```

   A **structure**, written `struct`, groups related values:

   ```cpp
   struct Config {
       uint32_t payload = 1024;
       uint32_t clients = 1;
   };
   ```

   An object named `c` of this type can hold both values. `c.payload` means “the payload setting inside `c`.”

   The project uses C++20, which is a version of the C++ language standard.

   C and C++ are related languages, but they are different languages. We use C++ for organization, memory ownership, and concurrency, while calling Linux networking interfaces that have C-style function signatures.

4. **The source code must be turned into an executable before it can run.**

   A computer does not directly execute our C++ source files.

   A **compiler** translates the source into machine instructions.

   A **linker** combines the translated pieces and required libraries into an executable program.

   A **library** is reusable code shared by other parts of a program.

   Your project produces two main executables:

   ```text
   netbench_server
   netbench_client
   ```

   We use **CMake** to describe how to build them.

   This command configures the build:

   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   ```

   Its parts mean:

   | Part | Meaning |
   |---|---|
   | `cmake` | Run the CMake tool |
   | `-S .` | The source directory is the current directory |
   | `-B build` | Put generated build files in a directory named `build` |
   | `-DCMAKE_BUILD_TYPE=Release` | Choose a build configuration intended for optimized execution |

   This command performs the build:

   ```bash
   cmake --build build -j 4
   ```

   `-j 4` allows up to four build jobs at once. **It does not mean four networking clients.** Building and running are separate activities.

   A **terminal** is the application where you enter these commands. The **shell** interprets them.

   When you write:

   ```bash
   ./build/netbench_client
   ```

   you are telling the shell to execute the program at that path.

   Here, `.` means the current directory, and `/` separates directory names.

5. **A running program is called a process.**

   The executable is a file. When you run that file, Linux creates a **process**.

   Our client and server run as separate processes.

   The **client** initiates the benchmark exchanges.

   The **server** waits for incoming communication and returns the data it receives.

   “Client” and “server” describe roles. They do not require two physical computers.

   By default, both processes run on your computer:

   ```text
   Your computer
   ┌──────────────────────────────────────────┐
   │                                          │
   │   Client process  ───────► Server process │
   │                   ◄───────               │
   │                                          │
   └──────────────────────────────────────────┘
   ```

   The server sends back the data it received. This behavior is called an **echo**.

   We use an echo because the client can answer two useful questions:

   > Did the data come back correctly?

   > How long did the complete exchange take?

6. **An IP address identifies a destination, and a port identifies an endpoint there.**

   An **IP address** is an address used by the networking system.

   Our default address is:

   ```text
   127.0.0.1
   ```

   This is a **loopback** address. Traffic sent there stays on the local machine.

   **Localhost** means the local computer; in our commands we use the numeric address directly.

   A **port** is a number used to distinguish networking endpoints. Our default server port is:

   ```text
   9000
   ```

   Therefore:

   ```text
   127.0.0.1:9000
   ```

   means “port 9000 on this computer.”

   The client also has a local port. Linux normally selects that port automatically.

   Two processes can communicate through Linux networking while running on the same computer. This exercises the networking software, but it does not measure the behavior of your Wi-Fi, Ethernet cable, router, or a remote computer.

   That is why our results are **local software/network-stack measurements**, rather than measurements of an Internet connection.

7. **TCP and UDP provide different ways to communicate.**

   A **protocol** is an agreed set of communication rules.

   **TCP** stands for Transmission Control Protocol.

   TCP establishes a connection and presents the application with an ordered stream of bytes. It handles mechanisms such as retransmission of missing network data. Communication can still fail; TCP does not guarantee that a disconnected or unreachable peer will eventually receive everything. These are properties of TCP itself, described in its [protocol specification](https://www.rfc-editor.org/rfc/rfc9293.html).

   **UDP** stands for User Datagram Protocol.

   UDP sends individual messages called **datagrams**. It does not provide TCP’s automatic delivery and ordering machinery. Datagrams may fail to arrive, arrive out of order, or be duplicated. Linux documents those semantics in [udp(7)](https://man7.org/linux/man-pages/man7/udp.7.html).

   The practical distinction for this project is:

   | Question | TCP | UDP |
   |---|---|---|
   | What does the application receive? | A stream of bytes | Individual datagrams |
   | Are our message boundaries automatically preserved? | No | Each datagram has a boundary |
   | Does the transport handle retransmission? | Yes, as part of TCP’s reliability mechanisms | Not automatically |
   | What extra work does our application need? | Define and read message boundaries | Identify echoes and account for missing ones |

   UDP is not automatically faster in every application. TCP is not automatically slower in every application. Measuring a specific workload is the purpose of the project.

8. **A socket is the interface our program uses to communicate.**

   A **socket** is an operating-system networking endpoint that an application can use.

   Our C++ code asks Linux to create and operate sockets through functions such as these:

   | Function | What our program asks Linux to do |
   |---|---|
   | `socket()` | Create a socket |
   | `bind()` | Attach a local address and port |
   | `listen()` | Prepare a TCP socket to receive connection requests |
   | `accept4()` | Obtain a socket for an incoming TCP connection |
   | `connect()` | Select/connect to a destination |
   | `send()` | Send bytes on a connected socket |
   | `recv()` | Receive bytes |
   | `sendto()` | Send a datagram |
   | `recvfrom()` | Receive a datagram |
   | `setsockopt()` | Change a socket setting |
   | `getsockopt()` | Read a socket setting |
   | `close()` | Release the socket descriptor |

   These are operating-system interfaces. **POSIX** is a family of standards for interfaces used by Unix-like operating systems; Linux provides these socket APIs and additional Linux-specific features.

   The TCP server follows this general sequence:

   ```text
   Create socket
        ↓
   Bind to 127.0.0.1:9000
        ↓
   Listen for connections
        ↓
   Accept a connection
        ↓
   Receive a message
        ↓
   Echo it back
        ↓
   Repeat until the connection ends
   ```

   The listening socket and the accepted connection socket have different jobs.

   The listening socket receives connection requests. The accepted socket handles the conversation with a particular client.

   Linux represents an open socket in our process using a **file descriptor**: a small integer handle. Our code passes that handle to functions such as `send()` and `recv()`.

   The descriptor is a handle to the socket, not the message itself.

9. **TCP requires us to define where each message starts and ends.**

   This is one of the most important ideas in the project.

   Suppose a sender performs these two operations:

   ```text
   Send "HELLO"
   Send "WORLD"
   ```

   The receiver cannot assume it will receive:

   ```text
   First receive:  "HELLO"
   Second receive: "WORLD"
   ```

   It might instead receive:

   ```text
   First receive:  "HEL"
   Second receive: "LOWOR"
   Third receive:  "LD"
   ```

   Or it might receive:

   ```text
   First receive: "HELLOWORLD"
   ```

   Those examples illustrate the same byte stream being delivered in different chunks.

   Therefore, our program needs a **framing strategy**: rules for identifying complete application messages.

   We put a fixed-size **header** before every payload.

   ```text
   Application message
   ┌───────────────────┬────────────────────────┐
   │ 24-byte header    │ Payload                 │
   └───────────────────┴────────────────────────┘
   ```

   Our header contains:

   | Field | Size | Purpose |
   |---|---:|---|
   | Magic/version | 4 bytes | Recognize our protocol format |
   | Payload length | 4 bytes | State the complete logical payload size |
   | Sequence number | 8 bytes | Identify the logical message |
   | Fragment index | 4 bytes | Identify a UDP fragment |
   | Fragment count | 4 bytes | State how many fragments belong to the message |

   These add up to 24 bytes.

   For TCP, fragment index is zero and fragment count is one.

   A **sequence number** is simply a message identifier that increases:

   ```text
   Message 0
   Message 1
   Message 2
   ...
   ```

   The server first reads the complete header, validates it, and then reads the exact payload length specified in it.

   Validation happens before allocating a potentially large buffer. A malformed header must not be allowed to request an arbitrary amount of memory.

   The header’s “magic” value is a format marker. It is not encryption or authentication.

10. **The header must have the same byte representation on different computers.**

   Computers can store multi-byte numbers in different byte orders.

   For example, a number occupying four bytes needs an agreed order for those bytes when transmitted.

   Our protocol uses **network byte order**, which places the most significant byte first.

   The payload length 1024 is encoded as:

   ```text
   00 00 04 00
   ```

   These numbers are shown in **hexadecimal**, a notation programmers often use to represent bytes.

   Rather than sending the raw memory occupied by a C++ `struct`, our code explicitly places each field into the header.

   Why?

   A compiler may insert unused spacing between fields, called **padding**. Different machines can also have different native byte ordering.

   Explicit encoding avoids making our wire format depend on those implementation details.

   **Encoding** converts our values into the agreed byte representation.

   **Decoding** converts received bytes back into values.

   This work lives in [protocol.cpp](../src/protocol.cpp).

11. **Sending or receiving TCP data can require several attempts.**

   Suppose we ask Linux to send 1,000 bytes.

   The return value from `send()` might say that it accepted only 300 bytes.

   That does not necessarily mean failure. It means our program must continue with the remaining 700 bytes.

   The basic idea is:

   ```text
   Start at byte 0
   Send as much as Linux accepts
   Advance by the number actually accepted
   Repeat until every byte has been accepted
   ```

   A simplified version looks like this:

   ```cpp
   size_t position = 0;

   while (position < total_size) {
       auto sent = send(
           socket_fd,
           data + position,
           total_size - position,
           MSG_NOSIGNAL
       );

       // The real code handles errors before advancing.
       position += sent;
   }
   ```

   This is a teaching sketch, not the full implementation.

   Some syntax in that sketch:

   | Syntax | Meaning |
   |---|---|
   | `size_t` | An integer type used for sizes |
   | `auto` | Ask the compiler to infer the variable’s type |
   | `data` | A pointer to the beginning of some bytes |
   | `data + position` | Start at an offset into those bytes |
   | `total_size - position` | How many bytes remain |
   | `+=` | Add a value to the existing value |

   A **pointer** holds the location of data in memory. It lets the networking function find the bytes to send or the memory where received bytes should be placed.

   Our actual `send_all()` function handles partial progress and errors.

   Our `recv_exact()` function does the corresponding receiving work: keep reading until the requested number of bytes has arrived, or report why that could not happen.

   Also, a successful `send()` means Linux accepted the bytes for sending. It does **not** mean the server application has already processed them.

   The validated echo provides stronger evidence that the complete application exchange happened.

12. **Buffers are temporary places where bytes wait.**

   A **buffer** is an area of memory used to hold data.

   Several different buffers appear in this project:

   | Buffer | Who manages it? | What it holds |
   |---|---|---|
   | Application transmit buffer | Our C++ program | Bytes being prepared for sending |
   | Application receive buffer | Our C++ program | Bytes returned by a receive operation |
   | Socket send buffer | Linux | Data queued for networking |
   | Socket receive buffer | Linux | Data waiting for the application to read |

   These are separate things.

   Increasing a C++ vector’s size does not automatically increase the kernel’s socket receive buffer.

   We use `std::vector<uint8_t>` for application buffers.

   We use:

   ```text
   SO_SNDBUF
   SO_RCVBUF
   ```

   to request socket send and receive buffer settings through `setsockopt()`.

   Small socket buffers can create **backpressure**: the program cannot keep adding data because the available space is limited.

   Bigger buffers can help some workloads, but they also consume resources and do not automatically improve performance.

   Linux can clamp requested sizes to system limits, and it reports buffer sizes using bookkeeping rules that include doubling requested values. We therefore record both requested and observed settings using `getsockopt()`. A reported doubled value should not be interpreted as exactly that much usable payload capacity. See [socket(7)](https://man7.org/linux/man-pages/man7/socket.7.html).

13. **Our program checks the data rather than assuming the transfer worked.**

   The client generates a deterministic byte pattern.

   **Deterministic** means the same inputs produce the same output every time.

   For each byte position `i`, the project generates a value using:

   ```cpp
   (i * 131 + 17) % 251
   ```

   `%` means the remainder after division.

   The particular numbers are not a networking standard. They create a repeatable pattern that is easy for our program to generate and compare.

   The process is:

   ```text
   Generate known bytes
         ↓
   Send them
         ↓
   Receive echoed bytes
         ↓
   Compare every returned payload byte with the expected byte
   ```

   This is an **integrity check**.

   Without it, a broken server could return incomplete or incorrect data while the benchmark still claimed to be fast.

   We also check message identifiers and lengths.

   These checks are inside the measured per-message work. Therefore, our reported round-trip time includes the work of validating the response.

14. **UDP needs a separate strategy for large logical messages and missing echoes.**

   An ordinary IPv4 UDP datagram cannot contain a 1 MiB application payload.

   To support the requested logical payload sizes, our application divides large UDP messages into **fragments**.

   Each fragment carries at most:

   ```text
   1,200 bytes of payload
   ```

   plus our 24-byte header.

   For example, a logical payload of 3,000 bytes becomes:

   ```text
   Fragment 0: 1,200 payload bytes
   Fragment 1: 1,200 payload bytes
   Fragment 2:   600 payload bytes
   ```

   Every fragment identifies its logical message, its fragment index, and the total fragment count.

   A 1 MiB message requires 874 fragments.

   Our workload uses **stop-and-wait** behavior for these fragments:

   ```text
   Send fragment 0 → wait for its echo
   Send fragment 1 → wait for its echo
   Send fragment 2 → wait for its echo
   ...
   ```

   The server echoes fragments individually. It does not need to retain an unbounded collection of partially received large messages.

   The client checks the returned fragment bytes and tracks which fragment identifiers have been seen.

   **Duplicate** means the same fragment echo is received more than once.

   **Reordered** means arrivals are observed in an order different from their identifiers.

   **Late or stale** means an echo does not belong to the currently active logical message.

   Duplicate echoes do not count as additional successful data. The tracker has tests for gaps, duplicates, and reordering, although the stop-and-wait workload gives little opportunity for natural reordering.

   Our loss metric is specifically **missing round-trip echoes**.

   For an arithmetic example:

   ```text
   Datagrams sent:          100
   Unique valid echoes:     95
   Missing echoes:           5

   Echo loss percentage = 5 / 100 × 100 = 5%
   ```

   That does not reveal whether a request disappeared, a response disappeared, or the response arrived too late.

   A logical message fails if it cannot obtain all required fragment echoes before its deadline. Fragments never sent after an abort do not enter the sent-datagram denominator.

   We do not derive a TCP packet-loss percentage from ordinary TCP reads. TCP hides retransmission details behind its byte-stream interface.

15. **A thread is a path of execution inside a process.**

   A process can contain multiple **threads**.

   Threads within the same process share memory, but each can execute its own sequence of instructions.

   In our client:

   ```bash
   --clients 4
   ```

   creates four client workers.

   They are four threads inside one client process, rather than four separately launched client programs.

   Each worker has its own socket and statistics.

   The TCP server uses one worker thread per accepted connection, up to the configured limit.

   **Concurrency** means multiple activities can be in progress.

   **Parallelism** means multiple activities are executing simultaneously, usually on different CPU cores.

   Concurrent workers may run in parallel, but Linux decides when and where they run. Creating four threads does not reserve four cores.

   Before measurement starts, our client workers synchronize using a **barrier**.

   A barrier is a meeting point: participating threads wait until everyone reaches it.

   Our startup sequence is:

   ```text
   Create worker threads
         ↓
   Workers create/connect sockets
         ↓
   All workers reach a barrier
         ↓
   Main thread records starting time and CPU usage
         ↓
   Workers are released to run the benchmark
   ```

   This keeps connection establishment out of the client measurement window.

   Shared server counters use **atomic** operations: operations designed to be safely accessed by multiple threads.

   The server also uses a **mutex**, which coordinates exclusive access, and a **condition variable**, which lets the accept loop sleep until worker capacity becomes available.

   These prevent the program from continuously checking for a free worker slot or racing against a worker that is finishing.

16. **Blocking and nonblocking describe what happens when an operation cannot proceed immediately.**

   **I/O** means input/output. Here, it mainly means sending and receiving bytes.

   With **blocking I/O**, a thread can wait inside an operation until progress becomes possible or its bounded wait ends.

   With **nonblocking I/O**, the operation returns promptly when it cannot currently progress.

   That return is commonly reported as `EAGAIN` or `EWOULDBLOCK`.

   Those names mean approximately:

   > This operation cannot proceed right now; try again when conditions change.

   Repeatedly retrying without waiting would waste CPU.

   Instead, our code uses `poll()` to wait for readiness:

   ```text
   Attempt operation
         ↓
   Cannot proceed yet
         ↓
   Ask poll() to wait for readiness
         ↓
   Retry the operation
   ```

   Readiness means an operation can make progress; it does not mean a complete application message is available. Linux describes this interface in [poll(2)](https://man7.org/linux/man-pages/man2/poll.2.html).

   Nonblocking does not mean “automatically faster.”

   Our nonblocking implementation still uses the same worker-thread architecture. It is not a single-thread event-driven server handling thousands of clients.

   We did not implement an `epoll` reactor. That would be a different architectural experiment.

17. **Timeouts, errors, and cleanup are part of correctness.**

   A **timeout** prevents a worker from waiting indefinitely.

   We use an absolute **deadline**:

   ```text
   Deadline = time the operation began + allowed duration
   ```

   That deadline stays the same even if only part of the message arrives.

   Otherwise, a peer could keep resetting the timeout by providing tiny amounts of data.

   Some important cases are:

   | Situation | What our program does |
   |---|---|
   | `EINTR` | An operation was interrupted by a signal; retry appropriately |
   | `EAGAIN` | Wait for readiness, then retry |
   | TCP receive returns zero | Treat it as the peer ending its sending side |
   | EOF inside a message | Report an incomplete-frame error |
   | Deadline expires | Record a timeout |
   | Invalid header or wrong returned bytes | Report an error |
   | TCP times out mid-frame | End that connection rather than assume its framing position is still known |

   Blocking socket operations use short kernel waits so they can notice shutdown requests. Those waits and Linux scheduling mean deadlines are not hard real-time guarantees.

   Linux can deliver **signals**, which notify a process about events.

   Pressing `Ctrl+C` normally produces `SIGINT`. The experiment runner uses `SIGTERM` to request server shutdown.

   Our signal handler sets a shutdown flag. Workers check it, finish or stop their work, and release their sockets.

   `MSG_NOSIGNAL` prevents a send to a closed peer from terminating the process through `SIGPIPE`; we handle the socket error instead.

   Resource cleanup uses a C++ pattern called **RAII**.

   The practical rule is:

   > An object owns a resource and releases it automatically when the object’s lifetime ends.

   Our `Fd` class owns a socket descriptor. Its destructor closes the descriptor.

   A **destructor** is cleanup code that runs when an object is destroyed.

   We prohibit copying `Fd` objects because two owners could both try to close the same descriptor. We allow **moving**, which transfers ownership and leaves the previous owner empty.

   This is why the code contains constructs such as:

   ```cpp
   Fd(const Fd&) = delete;
   ```

   It tells the compiler to reject attempts to copy the owner.

18. **We measure several different kinds of performance.**

   **Round-trip time**, abbreviated **RTT**, is the time from starting a request operation to receiving and validating its complete echo.

   ```text
   Client begins request
         ↓
   Request reaches server
         ↓
   Server sends echo
         ↓
   Client receives and validates echo
   ```

   We use:

   ```cpp
   clock_gettime(CLOCK_MONOTONIC_RAW, ...)
   ```

   This provides clock readings suitable for elapsed-time measurement without ordinary time-of-day clock adjustments affecting the difference. Its Linux behavior is documented in [clock_gettime(3)](https://man7.org/linux/man-pages/man3/clock_gettime.3.html).

   Our conversion is:

   ```text
   RTT = finish clock reading − start clock reading
   ```

   The units are:

   | Unit | Fraction of a second |
   |---|---:|
   | Millisecond, ms | 1/1,000 |
   | Microsecond, µs | 1/1,000,000 |
   | Nanosecond, ns | 1/1,000,000,000 |

   A reading expressed in nanoseconds does not guarantee nanosecond measurement accuracy.

   Our RTT includes application work such as preparing the current message, allocating buffers when selected, socket operations, waiting, and checking returned bytes. Initial worker setup is outside each message’s RTT but inside the overall trial window.

   We report several summaries of the successful RTT samples.

   **Mean** is the arithmetic average.

   **Median**, also called p50, describes the middle of the sorted samples.

   **p95** and **p99** describe the 95th and 99th percentiles.

   For a teaching example, suppose the times are:

   ```text
   10, 10, 10, 10, 100 microseconds
   ```

   The mean is 28 microseconds, while the median is 10.

   The average exposes the effect of the slow sample, while the median describes a more typical sample.

   Our percentile implementation sorts the values and uses the **nearest-rank** rule. With only five samples, both p95 and p99 select the maximum.

   That is why a p99 based on very few samples is not a dependable description of rare behavior.

   Timeouts are counted separately rather than inserted into the successful RTT list. If requests fail, successful-only latency can look deceptively good, so latency must be read alongside completion and timeout counts.

   We do not divide RTT by two and call it measured one-way latency. The two directions need not take equal time.

   **Goodput** measures successfully completed application payload per unit of elapsed time:

   ```text
   Goodput in Mbps =
       completed messages
       × payload bytes per message
       × 8
       ÷ elapsed seconds
       ÷ 1,000,000
   ```

   We count the application payload once, even though an echo sends those bytes back too. Headers and lower-level networking overhead are excluded.

   Therefore, goodput is not the total amount of traffic generated.

   **CPU utilization** measures how much processor time the client used during its measurement window.

   We obtain user and system CPU time using `getrusage()`.

   User CPU time is work executing the application’s instructions. System CPU time is kernel work charged to that process.

   ```text
   Client CPU percentage =
       CPU seconds used
       ÷ elapsed seconds
       × 100
   ```

   Under this convention, 100% means roughly one CPU core’s worth of execution. A multithreaded process can exceed 100%.

   A thread sleeping while waiting for data usually consumes little CPU during that wait.

   Server CPU is recorded over its own benchmark lifetime, including setup and shutdown. Its accounting window differs from the client’s active measurement window.

19. **The workload deliberately controls how quickly requests begin.**

   A **benchmark** is a controlled measurement workload.

   Ours is a request/echo benchmark with one logical request outstanding per worker.

   It does not continuously stream unlimited data.

   We use **pacing**, meaning we schedule request starts rather than sending as rapidly as possible.

   Two settings constrain the aggregate request budget:

   ```bash
   --rate 10000
   --mbps 100
   ```

   These mean a nominal budget of:

   ```text
   10,000 logical requests per second
   100 megabits of request payload per second
   ```

   The program uses whichever constraint is tighter, then divides that budget among the client workers.

   For example, with 1,024-byte payloads:

   ```text
   10,000 × 1,024 × 8
       = 81,920,000 bits per second
       = 81.92 Mbps
   ```

   So the request-count budget is already more restrictive than the 100 Mbps setting.

   Sleeping and scheduling overhead can reduce the achieved rate further.

   The program uses `nanosleep()` to wait between scheduled starts. It does not constantly spin on the CPU.

   For UDP, fragment pacing occurs inside a logical message. Therefore, large-message UDP RTT includes intentional waiting between fragments.

   This explains why the measured 1 MiB UDP logical RTT was about 83.9 milliseconds. That is largely workload policy, not evidence that the underlying transport inherently needs 83.9 milliseconds to deliver a large collection of bytes.

   The limits are scheduling budgets, not strict wire-rate shaping.

   The first message can start immediately, and a started message can finish beyond the configured start-duration boundary. Large messages in short windows can therefore produce goodput slightly above the nominal budget.

   For example:

   ```text
   Eight completed 1 MiB messages in 0.6 seconds
       ≈ 111.8 Mbps
   ```

   That arithmetic explains an observed result without implying a link-capacity discovery.

   Our first timing implementation also ended some duration-limited runs before waiting out the final pacing gap. We corrected that and preserved the original records with an explanation.

20. **The two optimization options change different pieces of work.**

   An **optimization** is a change intended to improve some cost: time, CPU work, memory use, or another useful property.

   A **baseline** is the reference implementation used for comparison.

   An optimization is only demonstrated for the conditions where it was measured.

   The first change was **coalescing writes**.

   Baseline:

   ```text
   Send header
   Send payload
   ```

   Coalesced strategy:

   ```text
   Prepare header and payload together
   Send the complete frame
   ```

   Why can this help?

   TCP has a policy called the **Nagle algorithm**, intended to reduce inefficient small transmissions. `TCP_NODELAY` disables that policy. Linux documents the option in [tcp(7)](https://man7.org/linux/man-pages/man7/tcp.7.html).

   In our baseline, sending a small header separately from a small body created an interaction consistent with Nagle and delayed acknowledgments.

   An **acknowledgment**, or ACK, is TCP’s notification about received data. It is different from our application’s echo response.

   The server application needed the body to complete the message, while TCP’s small-write behavior could delay delivery of that body.

   Our observations supported that explanation, although we did not capture network packets to prove the exact internal timer behavior.

   In the longer 64-byte tests:

   | Configuration | Mean RTT |
   |---|---:|
   | Split writes, NODELAY off | About 40,801 µs |
   | Coalesced writes, NODELAY off | About 27.31 µs |

   Much of that dramatic change removed an avoidable interaction in our own application’s write pattern.

   With NODELAY already enabled, the difference was smaller:

   | Configuration | Mean RTT |
   |---|---:|
   | Split writes, NODELAY on | About 33.05 µs |
   | Coalesced writes, NODELAY on | About 26.90 µs |

   Coalescing does not guarantee one TCP packet. Also, `send_all()` may still need multiple underlying sends if Linux accepts only part of a large frame.

   The second change was **reusing application buffers**.

   In allocate mode, each message creates fresh working vectors, initializes storage, and copies the payload into the transmit frame.

   In reuse mode, the worker retains its buffers, prepares invariant data once, and updates the parts that change.

   This reduces a combination of allocation, initialization, and copying work. Our experiment does not isolate the allocator alone.

   In the longer 1 MiB TCP comparison:

   | Measurement | Allocate | Reuse |
   |---|---:|---:|
   | Mean RTT | About 1,203 µs | About 967 µs |
   | Client CPU time per completed message | About 977 µs | About 906 µs |

   That is approximately a 19.6% RTT reduction and a 7.2% CPU-per-message reduction.

   The trade-off is retaining memory for the connection’s lifetime.

   Reuse is not “zero-copy networking.” Socket transfer and integrity checking still require work.

   Goodput stayed similar because the workload was paced. Completing an individual exchange sooner does not necessarily mean more exchanges are allowed to start.

21. **We repeated experiments because one run is not enough.**

   Results can vary because of background programs, CPU scheduling, processor frequency, memory state, and other activity.

   A **trial** is one execution of a selected benchmark configuration.

   A **configuration** is the collection of settings for that trial.

   An **experiment matrix** is a collection of configurations covering combinations of settings.

   For example, the optimization matrix combines:

   ```text
   3 payload sizes
   × 2 NODELAY settings
   × 2 write strategies
   × 2 buffering strategies
   = 24 configurations
   ```

   Five repetitions produce:

   ```text
   24 × 5 = 120 measured trials
   ```

   The saved campaign contains:

   | Dataset | Measured trials | Purpose |
   |---|---:|---|
   | Baseline | 57 | Establish behavior and discover problems |
   | Comparison | 120 | Compare optimization factors |
   | Survey | 84 | Investigate transport, payload, buffers, concurrency, and I/O mode |
   | Confirmation | 40 | Repeat selected findings using longer runs |
   | Total | **301** | |

   Warmup runs are stored separately.

   The Python runner starts a fresh server for each trial, waits for readiness, runs the client, collects output, and shuts the server down.

   It requests server port zero, which means “Linux, choose an available port.” The server reports the selected port to the runner.

   This avoids guessing an unused port and racing another process for it.

   The runner also records the compiler, operating-system details, CPU information, selected settings, commands, and environment.

   Trial order is randomized with a recorded **seed**. A seed makes the chosen ordering reproducible; it does not make operating-system timing perfectly reproducible.

   Each dataset contains a source archive and SHA-256 file fingerprints. A fingerprint helps identify exactly which bytes produced the results.

   Our longer confirmation runs found a smaller buffer-reuse benefit than the short runs. Both are reported.

   Other findings were also preserved:

   - Nonblocking mode did not consistently improve goodput here and used more client CPU in the surveyed pairs.
   - Larger TCP socket buffers reduced 16 KiB RTT when the original buffers were small, but further increases did little.
   - Coalescing alone was not a consistent large-payload improvement.
   - Zero missing UDP echoes on loopback does not establish behavior under congestion or on a real network.

22. **Tests check correctness; experiments investigate performance.**

   These are different activities.

   A **test** asks:

   > Did the program behave according to a specific expectation?

   An **assertion** checks that expectation.

   For example:

   ```text
   Expected: receiving a message in several chunks still reconstructs the right data.
   Actual: compare the received bytes with the known expected bytes.
   ```

   A **unit test** checks a smaller component, such as header encoding or sequence tracking.

   An **integration test** checks several parts working together, such as real client and server processes communicating through sockets.

   We use GoogleTest for C++ tests and Python’s `unittest` for integration tests.

   The suite includes 11 C++ tests and 16 integration test methods, with additional combinations exercised inside some methods.

   They check things such as partial reads and writes, malformed messages, timeouts, connection failures, duplicate UDP echoes, concurrent clients, cleanup, and repeated reconnects.

   Two real bugs found during development illustrate why tests matter.

   First, a server at its one-worker limit could reject a reconnect before noticing the previous worker had finished. The final server waits for capacity and leaves new connections in the bounded listen backlog.

   Second, EOF after a message had begun could be misclassified as a normal close. The server now tracks whether it is inside a frame and reports incomplete-frame closure correctly.

   We also ran **sanitizers**.

   AddressSanitizer instruments the program to detect certain memory-access errors, such as accessing memory outside an allocation or after it has been released.

   UndefinedBehaviorSanitizer checks certain operations whose behavior is not valid under C++ rules.

   These tools help find bugs, but they do not prove that every possible bug is absent.

   They also add overhead, so sanitizer builds are for correctness checking, not performance comparisons.

   The local release and sanitizer test runs passed. The repository also has a GitHub Actions workflow configured for future remote CI runs; that configuration is separate from the local executions.

23. **The files in the repository each have a specific job.**

   | File or directory | What it contains |
   |---|---|
   | [CMakeLists.txt](../CMakeLists.txt) | Build instructions |
   | [client.cpp](../src/client.cpp) | Client workers, pacing, sending, receiving, validation, and timing |
   | [server.cpp](../src/server.cpp) | Server startup, accepting connections, echoing, and worker management |
   | [common.cpp](../src/common.cpp) | Shared socket operations, settings, deadlines, clocks, and cleanup |
   | [protocol.cpp](../src/protocol.cpp) | Header encoding, decoding, payload generation, and sequence tracking |
   | [metrics.cpp](../src/metrics.cpp) | Combining statistics and generating output |
   | `include/netbench/` | Declarations describing the shared components |
   | `tests/` | Automated correctness checks |
   | `scripts/run_experiments.py` | Starts and records experiment matrices |
   | `scripts/analyze.py` | Creates summaries and plots |
   | `scripts/trace_writes.py` | Optional syscall diagnostics using `strace` |
   | `results/` | Actual trial records, metadata, source snapshots, plots, and validation logs |
   | `docs/` | Technical explanations and analysis |

   `.cpp` files contain C++ implementations.

   `.hpp` files commonly contain declarations: descriptions of types and functions that other files can use.

   `#include` makes those declarations available while compiling. `#pragma once` prevents the same header from being included repeatedly in a way that causes duplicate definitions.

   The code uses `namespace nb` to group our names and reduce collisions with names from other libraries.

   A few more C++ constructs you will encounter are:

   | Construct | Meaning in this project |
   |---|---|
   | `const` | Something should not be modified through that declaration |
   | A reference, such as `Config&` | Another name for an existing object, rather than a separate copied object |
   | `std::span` | A view of existing contiguous data without owning or copying it |
   | `std::unique_ptr` | Exclusive ownership of an allocated object |
   | `std::thread` | A running worker thread |
   | `join()` | Wait for a thread to finish |
   | `std::jthread` | A thread-owning type that joins automatically when destroyed |
   | `try` / `catch` | Run operations and handle exceptions that report failure |
   | `throw` | Report a failure by raising an exception |
   | `static_cast<double>(...)` | Convert a value to a fractional-number type for calculations |

24. **The output is a record of what happened, not just a speed score.**

   **JSON** is a structured text format using named fields.

   **CSV** is a table-like format with comma-separated values that spreadsheet and analysis tools can read.

   Here is a rounded subset from an actual saved 64-byte TCP confirmation trial:

   ```json
   {
     "status": "ok",
     "elapsed_s": 3.0001,
     "attempted_messages": 19371,
     "completed_messages": 19371,
     "goodput_mbps": 3.3059,
     "rtt_mean_us": 26.1321,
     "rtt_median_us": 23.444,
     "rtt_p95_us": 40.997,
     "rtt_p99_us": 57.518,
     "client_cpu_percent": 7.9719,
     "errors": 0,
     "timeouts": 0
   }
   ```

   Read it as:

   > Over approximately three seconds, this trial completed 19,371 validated message exchanges. The mean application round-trip time was about 26.13 microseconds. The client used about 7.97% of one core’s processing capacity over the window. No errors or timeouts were recorded.

   The goodput is about 3.31 Mbps because each request carried only 64 payload bytes and request starts were paced.

   That is not evidence that your computer’s network interface has a maximum speed of 3.31 Mbps.

   The saved “raw results” are raw per-trial outputs and logs. The program collects individual RTTs in memory to calculate statistics, but it does not save every individual RTT timestamp as a separate record.

   The plotting script groups matching configurations and compares their trial summaries.

   Graphs show means and observed minimum-to-maximum trial ranges. Those ranges are not statistical confidence intervals.

   The [technical analysis](ANALYSIS.md) explains which findings the data supports and which conclusions would go beyond it.

25. **You can now follow one complete run from command to result.**

   Start in the project directory and build:

   ```bash
   cd /home/aarush/Work/net_benchmark

   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j 4
   ```

   `cd` changes the current directory.

   Start the server in one terminal:

   ```bash
   ./build/netbench_server \
     --protocol tcp \
     --clients 1 \
     --duration 30 \
     --buffering reuse
   ```

   The `\` at the end of a line tells the shell that the command continues on the next line.

   The server creates a socket, binds its address and port, listens, and waits for a client. Its duration is 30 seconds.

   In a second terminal, run:

   ```bash
   cd /home/aarush/Work/net_benchmark

   ./build/netbench_client \
     --protocol tcp \
     --payload 1024 \
     --clients 1 \
     --duration 2 \
     --messages 10000 \
     --write coalesced \
     --buffering reuse \
     --nodelay 1
   ```

   Each option has a concrete meaning:

   | Option | Meaning |
   |---|---|
   | `--protocol tcp` | Use TCP sockets |
   | `--payload 1024` | Each logical request contains 1,024 application bytes |
   | `--clients 1` | Create one client worker |
   | `--duration 2` | Stop starting new messages after roughly two seconds |
   | `--messages 10000` | Allow at most 10,000 messages for that worker |
   | `--write coalesced` | Put the header and payload together for transmission |
   | `--buffering reuse` | Retain application buffers between requests |
   | `--nodelay 1` | Enable TCP_NODELAY |

   The message and duration limits compete: the worker stops when the relevant first limit is reached. A message already started may finish or time out afterward.

   Internally, the client:

   ```text
   Reads and validates command-line options
         ↓
   Creates and connects a socket
         ↓
   Synchronizes the measurement start
         ↓
   Prepares known payload bytes and reusable buffers
         ↓
   Waits for a scheduled request start
         ↓
   Creates the message header
         ↓
   Sends the complete frame
         ↓
   Receives the complete echo
         ↓
   Checks the header and every payload byte
         ↓
   Records the RTT
         ↓
   Repeats until a limit or failure
         ↓
   Combines statistics and prints JSON
         ↓
   Releases its resources
   ```

   If the server’s 30 seconds have elapsed before you run the client, restart it.

   Run the correctness checks with:

   ```bash
   ctest --test-dir build --output-on-failure
   ```

   To study the behavior yourself, change **one setting at a time**. For example, compare payloads of `64` and `65536` while keeping the other settings unchanged. Then repeat both configurations.

   Your first learning exercise should be to predict what each command asks the program to do, run it, and explain each output field in your own words. That connects the networking concepts to something you can directly observe on your computer.
