with qn1 as (with qn3 as (select * from qn2) select * from qn3),
     qn2 as (select 1)
select * from qn1;
