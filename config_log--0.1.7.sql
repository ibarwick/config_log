
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION config_log" to load this file. \quit

CREATE TABLE pg_settings_log AS
 SELECT name,
        setting,
        unit,
        sourcefile,
        sourceline,
        CAST('INSERT' AS VARCHAR(6)) AS op,
        CURRENT_TIMESTAMP AS recorded_ts
   FROM pg_settings WHERE source='configuration file';

CREATE OR REPLACE VIEW pg_settings_log_current
  AS  SELECT psl.*
        FROM pg_settings_log psl
   LEFT JOIN pg_settings_log psl_ref
          ON (psl.name = psl_ref.name
         AND psl.recorded_ts < psl_ref.recorded_ts)
       WHERE psl_ref.name IS NULL;

CREATE OR REPLACE FUNCTION pg_settings_logger()
  RETURNS BOOLEAN
  LANGUAGE plpgsql
AS $$
DECLARE
  changed BOOLEAN := FALSE;
  settings_rec RECORD;
BEGIN
  FOR settings_rec IN
    WITH pg_settings_log_current AS (
      SELECT *
        FROM pg_settings_log_current
    ORDER BY name
    )
    SELECT 'UPDATE' AS op,
           ps.name,
           ps.setting,
           ps.unit,
           ps.sourcefile,
           ps.sourceline
      FROM pg_settings ps
INNER JOIN pg_settings_log_current psl ON (psl.name=ps.name AND psl.setting != ps.setting)
     WHERE ps.source ='configuration file'
        UNION
    SELECT 'INSERT' AS op,
           ps.name,
           ps.setting,
           ps.unit,
           ps.sourcefile,
           ps.sourceline
      FROM pg_settings ps
     WHERE ps.source ='configuration file'
       AND NOT EXISTS (SELECT NULL
                         FROM pg_settings_log_current psl
                        WHERE psl.name = ps.name
                      )
        UNION
    SELECT 'DELETE' AS op,
           psl.name,
           psl.setting,
           psl.unit,
           psl.sourcefile,
           psl.sourceline
      FROM pg_settings_log_current psl
     WHERE EXISTS (SELECT NULL
                         FROM pg_settings ps
                        WHERE ps.name = psl.name
                          AND ps.source ='default'
                      )
       AND psl.op != 'DELETE'

    LOOP
      INSERT INTO pg_settings_log
                 (name,
                  setting,
                  unit,
                  sourcefile,
                  sourceline,
                  op,
                  recorded_ts
                 )
           VALUES(settings_rec.name,
                  settings_rec.setting,
                  settings_rec.unit,
                  settings_rec.sourcefile,
                  settings_rec.sourceline,
                  settings_rec.op,
                  CURRENT_TIMESTAMP
                 );
      changed = TRUE;
    END LOOP;
    RETURN changed;
  END;
$$;

REVOKE ALL ON pg_settings_log FROM public;
REVOKE ALL ON pg_settings_log_current FROM public;
REVOKE ALL ON FUNCTION pg_settings_logger() FROM public;
