with recursive qn as
  (select 1 as a from dual union all
   select max(a) from qn)
select * from qn;
