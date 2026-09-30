select count(*) from t1_30237_bool
  where ((A OR B) OR C) != (A OR (B OR C));
