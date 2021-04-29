# TCP-Quiz

Online quiz system that uses TCP for inter-process communication. The system consists of two programs: client and server. Each question is a string composed of
printable ASCII characters. Questions can not be longer than 2000 bytes. When sending question over the network each question ends with a \0 character to let the client know that this is the end of the question.

## Getting Started

**Server**
The server starts with four parameters: address, port, maximum number of clients, and the path to the question file.

**Client**
Client starts with an even number of arguments - a list of servers (address and port argument pairs).
