WITH qn2 AS (SELECT a FROM qn WHERE a IS NULL or a>0),
qn AS (SELECT b as a FROM t1)
SELECT qn2.a  FROM qn2;
