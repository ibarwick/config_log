MODULES = config_log
OBJS = config_log.o

EXTENSION 	= config_log
EXTVERSION 	= $(shell grep default_version $(EXTENSION).control | \
			  sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

DATA = config_log--0.1.sql

PG_CONFIG	= pg_config

# verify version is 9.3 or later

PG93        = $(shell $(PG_CONFIG) --version | grep -qE " 8\.| 9\.0 | 9\.1| 9\.2" && echo no || echo yes)

ifeq ($(PG93),no)
$(error Requires PostgreSQL 9.3 or later)
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)


