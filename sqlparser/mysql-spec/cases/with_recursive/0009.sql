with recursive qn as (select * from qn2),
               qn2 as (select * from qn)
select * from qn;
