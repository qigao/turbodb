select count(*) from t1_30237_bool
  where ((A AND B) OR C) != (A AND B OR C);
