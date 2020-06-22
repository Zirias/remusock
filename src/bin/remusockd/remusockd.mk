remusockd_MODULES:= main daemon log syslog config util event service \
	threadpool connection server client protocol
remusockd_LDFLAGS:= -pthread
$(call binrules, remusockd)

