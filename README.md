## remusock

This package contains a single and simple daemon, `remusockd`, allowing to
access a Unix domain socket on a remote machine.

```
Usage: remusockd [-cfnv] [-b address]
                [-g group] [-m mode] [-p pidfile]
                [-r remotehost] [-u user] socket port

        -b address     when listening, only bind to this address
                       instead of any
        -c             open unix domain socket as client
        -f             run in foreground
        -g group       group name or id for the server socket,
                       if a user name is given, defaults to the
                       default group of that user
        -m mode        permissions for the server socket in octal,
                       defaults to 600
        -n             numeric hosts, do not resolve remote addresses
        -p pidfile     use `pidfile' instead of compile-time default
        -r remotehost  connect to `remotehost' instead of listening
        -u user        user name or id for the server socket
                       when started as root, run as this user
        -v             verbose logging output

        socket         unix domain socket to open
        port           TCP port to connect to or listen on
```

### Limitations

* *No encryption, no authentication!* — use this tool only in trusted networks
* event loop is based on `pselect()`, so this doesn't scale to huge numbers
  of connections

### Features

* Multiple socket connections tunnelled through a single TCP connection
* TCP can work in either direction between socket server and socket client
* TCP connections are monitored, the client side attempts to automatically
  restore a lost connection

### Building

1. Clone the source from git:

        git clone https://github.com/Zirias/remusock
        cd remusock

2. (optional) check out a specific release:

        git checkout v1.1

3. get [`zimk`](https://github.com/Zirias/zimk) (the GNU make based build
   system)

        git submodule update --init

4. build it

        make -j4 strip

   this assumes `make` is GNU make. You'll have to type `gmake` instead on
   a BSD system, for example.

