SELECT
  count(*) AS matched_rows,
  sum(round(left_fact.value * 1000000)::BIGINT) AS total_micro_units
FROM fact AS left_fact
JOIN fact AS right_fact USING (event_id)
WHERE right_fact.value >= 0.50
