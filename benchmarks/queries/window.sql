SELECT event_id, drug_id, value, row_number() OVER (
  PARTITION BY drug_id
  ORDER BY value DESC, event_id
) AS rank_within_drug
FROM fact
