select count(*) from (
select                      a,b from t1  union all select a,b from t2) q;
