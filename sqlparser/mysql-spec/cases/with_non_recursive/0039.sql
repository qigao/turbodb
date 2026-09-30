WITH qn AS (SELECT a FROM t1), qn2 AS (SELECT a FROM qn)
SELECT * from qn2;
