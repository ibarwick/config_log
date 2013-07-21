config_log 0.1.5
================

config_log is an experimental extension which implements a custom
background worker to monitor postgresql.conf and record any changes to
a database table.

config_log requires PostgreSQL 9.3 or later and will not work in earlier
versions.

To build config_log, just do this:

    make
    make install

If you encounter an error such as:

    "Makefile", line 8: Need an operator

You need to use GNU make, which may well be installed on your system as
`gmake`:

    gmake
    gmake install
    gmake installcheck

If you encounter an error such as:

    make: pg_config: Command not found

Be sure that you have `pg_config` installed and in your path. If you used a
package management system such as RPM to install PostgreSQL, be sure that the
`-devel` package is also installed. If necessary tell the build process where
to find it:

    env PG_CONFIG=/path/to/pg_config make && make install

Once config_log is compiled and installed, it needs to be added to a database
in the PostgreSQL database cluster whose postgresql.conf you wish to monitor.
It can only be installed in one database within the cluster.

By default, config_log expects to be installed into the default 'postgres' 
database's 'public' schema. The default values can be overridden by changing 
the following GUC settings:

    config_log.database = 'some_other_db'
    config_log.schema   = 'different_schema'

Note that the schema must appear in the superuser's search path.

Connect to the target database as a superuser and execute:

    CREATE EXTENSION config_log

This will install the database objects required for this extension.

Following this step, the module must be added to postgresql.conf's 
'shared_preload_libraries' parameter and the server restarted.

Log output
----------

config_log will produce some log output, prefixed by "config_log:".

Following a successful startup, it will output the following:

    LOG:  config_log: initialized, database objects validated
    LOG:  config_log: pg_settings_logger() executed
    LOG:  config_log: No configuration changes detected

Following a SIGHUP, it will output something like the following:

    LOG:  received SIGHUP, reloading configuration files
    LOG:  parameter "temp_buffers" changed to "64MB"
    LOG:  config_log: received sighup
    LOG:  config_log: pg_settings_logger() executed
    LOG:  config_log: Configuration changes recorded


Usage
-----

For more information please see these blog posts:

- [sql-info.de: Custom Background Worker: a practical example](http://sql-info.de/postgresql/notes/custom-background-worker-bgw-practical-example.html)
- [sql-info.de: Logging changes to postgresql.conf](http://sql-info.de/postgresql/notes/logging-changes-to-postgresql-conf.html)

Links
-----

- config_log repository at [Github](https://github.com/ibarwick/config_log)
- config_log project page at [PGXN](http://www.pgxn.org/dist/config_log/)
- [PostgreSQL documentation](http://www.postgresql.org/docs/9.3/static/index.html): [Background Worker Processes](http://www.postgresql.org/docs/9.3/static/bgworker.html)