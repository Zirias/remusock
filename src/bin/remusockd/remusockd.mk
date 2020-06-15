remusockd_MODULES:= main daemon log syslog config util event service \
	server sockclient
$(call binrules, remusockd)

