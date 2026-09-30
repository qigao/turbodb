WITH qn AS (SELECT b as a FROM t1),
qn2 AS (SELECT c FROM t1 WHERE a IS NULL or a>0)
SELECT qn.a, qn2.c  FROM qn, qn2;
