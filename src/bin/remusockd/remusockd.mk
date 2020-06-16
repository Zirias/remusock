remusockd_MODULES:= main daemon log syslog config util event service \
	connection server client protocol
$(call binrules, remusockd)

