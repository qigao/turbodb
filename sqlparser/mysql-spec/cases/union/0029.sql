select 't1',b,count(*) from t1 group by b UNION select 't2',b,count(*) from t2 group by b;
