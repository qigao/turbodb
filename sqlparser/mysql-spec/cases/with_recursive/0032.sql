with recursive qn as
  (select 1 from qn union all select 1 from qn)
select * from qn;
