A Passive packet forwarder
===

Before the incoming packets can be forwarded, a connection from slave to master is made. This way packets can be forwarded into a nat'd environment.

server.c is the forwarder, which support forwarding tcp/udp over tcp/udp; client.c is a reference client implementation, currently only supports tcp over tcp.
