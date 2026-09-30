with recursive qn as
  (select rank() over (order by b) as a from t1 union
   select a from qn)
select * from qn;
