CREATE TABLE t1
 (
 place_id int (10) unsigned NOT NULL,
 shows int(10) unsigned DEFAULT '0' NOT NULL,
 ishows int(10) unsigned DEFAULT '0' NOT NULL,
 ushows int(10) unsigned DEFAULT '0' NOT NULL,
 clicks int(10) unsigned DEFAULT '0' NOT NULL,
 iclicks int(10) unsigned DEFAULT '0' NOT NULL,
 uclicks int(10) unsigned DEFAULT '0' NOT NULL,
 ts timestamp,
 PRIMARY KEY (place_id,ts)
 );
