# block_access extension

MODULES = block_access
PGFILEDESC = "block_access - control access based on time"
#DOCS = README.md

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
