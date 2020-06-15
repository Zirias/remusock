remusockd_MODULES:= main daemon log syslog config util event service \
	tcpserver sockserver sockclient
$(call binrules, remusockd)

