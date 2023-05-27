remusockd_MODULES:=	config \
			main \
			protocol

remusockd_PKGDEPS:=	posercore

$(call binrules, remusockd)
