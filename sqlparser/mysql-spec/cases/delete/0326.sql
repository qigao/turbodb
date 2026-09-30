DELETE FROM db1.t1 alias USING db1.t1, db2.t1 alias WHERE db1.t1.a = alias.a;
