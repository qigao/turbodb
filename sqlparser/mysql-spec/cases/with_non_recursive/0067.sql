with qn0 as (select 1), qn1 as (select * from qn0), qn2 as (select 1), qn3 as (select 1 from qn1, qn2) select 1 from qn3;
