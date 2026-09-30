select count(*) from t1_30237_bool
  where ((A AND B) AND C) != (A AND (B AND C));
