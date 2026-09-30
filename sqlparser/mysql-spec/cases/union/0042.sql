(select sql_calc_found_rows  a,b from t1 limit 2)  union all (select a,b from t2 order by a) limit 2;
