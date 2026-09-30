with qn as
  (with qn2 as (select "qn2" as a from t1) select "qn", a from qn2)
select * from qn;
