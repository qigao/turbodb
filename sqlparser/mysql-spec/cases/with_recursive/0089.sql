with recursive qn as
  (select 1 as a from dual group by a union all
   select a+1 from qn where a<3)
select * from qn;
