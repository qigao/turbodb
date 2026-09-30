with recursive qn as
  (select 1 from dual union all
   select 1 from dual)
select * from qn;
