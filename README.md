## remusock

This package contain a single and simple daemon, `remusockd`, allowing to
access a Unix domain socket on a remote machine.

```
Usage: remusockd [-cfv] [-b address]
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

