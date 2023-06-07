## remusock

This package contains a single and simple daemon, `remusockd`, allowing to
access a Unix domain socket on a remote machine.

```
Usage: remusockd [-Vcfntv] [-C CAfile] [-H hash[:hash...]]
		[-b address] [-g group] [-m mode] [-p pidfile]
		[-r remotehost] [-u user] socket port [cert key]

	-C CAfile      A file with one or more CA certificates in
	               PEM format. When listening, require a client
	               certificate issued by one of these CAs.
	-H hash[:...]  One or more SHA-512 hashes (128 hex digits).
	               When listening, require a client certificate
	               matching one of these hashes (fingerprints).
	-V             When connecting to a remote host with TLS,
	               don't verify the server certificate
	-b address     when listening, only bind to this address
	               instead of any
	               (can be given up to 4 times)
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
	-t             Enable TLS. This is implied when a cert and
	               key, or -C or -H are given. When listening
	               with TLS enabled, cert and key are required
	               and must match the hostname used to connect to
	               this instance.
	-u user        user name or id for the server socket
	               when started as root, run as this user
	-v             verbose logging output

	socket         unix domain socket to open
	port           TCP port to connect to or listen on
	cert           Certificate to use in PEM format
	key            Private key of the cert in PEM format
```

### Limitations

* event loop is based on `pselect()`, so this doesn't scale to huge numbers
  of connections

### Features

* Multiple socket connections tunnelled through a single TCP connection
* TCP can work in either direction between socket server and socket client
* TCP connections are monitored, the client side attempts to automatically
  restore a lost connection
* Optional TLS support with flexible validation of allowed client
  certificates, either by SHA-512 fingerprints of allowed certificates or by
  requiring specific issuing CAs, or both. Use this if the connection must
  cross an untrusted network.

### Building

To build `remusock`, you will need to have
[poser](https://github.com/Zirias/poser) installed, currently at least in
version `1.1`.

To build a release version, just extract the source tarball (e.g.
`remusock-2.0.txz`) and run this in the source directory:

    make -j4 strip

The result can be installed as the super-user (root) with

    make install

### FreeBSD port

There's a FreeBSD port in my local ports tree (caution, it's rebased all the
time) here: https://github.com/Zirias/zfbsd-ports/tree/local/net/remusock

It contains an init-script that allows easy configuration in `/etc/rc.conf`.
Here's what I use to make dovecot's auth socket available on a remote machine:

```
remusock_enable="YES"
remusock_socket="/var/run/dovecot/auth-client"
remusock_user="dovecot"
remusock_remotehost="some.host.example"
remusock_sockclient="YES"
remusock_cert="/usr/local/etc/remusock/remusock.crt"
remusock_key="/usr/local/etc/remusock/remusock.key"
```

### Linux/systemd

Remusock doesn't integrate with `systemd` (and never will), but it's of course
possible to create a "systemd unit" for it. I recommend to hardcode all the
command line arguments you need and use the "forking" model.

