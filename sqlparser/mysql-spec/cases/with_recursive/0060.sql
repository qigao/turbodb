with recursive qn as
  (select max(b) as a from t1 union
   select a from qn)
select * from qn;
