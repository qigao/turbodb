select sql_calc_found_rows  a,b from t1  union all select a,b from t2 limit 2;
