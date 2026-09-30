with recursive qn as
  (select 1 from qn union all
   select 1 from dual)
select * from qn;
