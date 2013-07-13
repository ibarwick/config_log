config_log 0.1.4
================

config_log is an experimental extension which implements a custom
background worker to monitor postgresql.conf and record any changes to
a database table.

config_log requires PostgreSQL 9.3 or later and will not work
in earlier versions.

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

    env PG_CONFIG=/path/to/pg_config make &&  make install

Once config_log is compiled and installed, it needs to be added to a database
in the PostgreSQL database cluster whose postgresql.conf you wish to monitor.
It can only be installed in one database within the cluster.

At the moment config_log is hard-wired to be installed in the default 'postgres'
database. Connect to this database as a superuser and execute:

    CREATE EXTENSION config_log

This will install the database objects required for this extension.

Following this step, the module must be added to postgresql.conf's 
'shared_preload_libraries' parameter and the server restarted.

Usage
-----

For more information please see these blog posts:

- http://sql-info.de/postgresql/notes/custom-background-worker-bgw-practical-example.html
- http://sql-info.de/postgresql/notes/logging-changes-to-postgresql-conf.html

Links
-----

- [Github](https://github.com/ibarwick/config_log)
- [PGXN](http://www.pgxn.org/dist/config_log/)
