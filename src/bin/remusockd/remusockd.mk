remusockd_MODULES:=	config \
			main \
			protocol \
			remusock \
			tcpclient \
			tcpserver

remusockd_PKGDEPS:=	posercore

$(call binrules, remusockd)
