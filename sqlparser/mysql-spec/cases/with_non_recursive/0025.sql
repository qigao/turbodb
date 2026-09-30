WITH qn AS (SELECT b as a FROM t1)
SELECT qn.a, qn2.a  FROM qn, qn as qn2;
