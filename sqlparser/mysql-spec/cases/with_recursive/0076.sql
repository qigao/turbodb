with recursive qn as
  (select b as a from t1 union
   select rank() over (order by a) from qn)
select * from qn;
