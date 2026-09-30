with recursive qn as
  (select b as a from t1 union
   select max(a) from qn)
select * from qn;
