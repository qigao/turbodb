explain (select a,b from t1 limit 2)  union all (select a,b from t2 order by a limit 1) order by b desc;
