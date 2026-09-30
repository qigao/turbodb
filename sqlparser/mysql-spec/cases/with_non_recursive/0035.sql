WITH qn AS (SELECT 10*a as a FROM t1),
qn2 AS (SELECT 3*a FROM qn)
SELECT * from qn2;
