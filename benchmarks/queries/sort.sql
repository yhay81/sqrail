SELECT event_id, drug_id, event_date, value, payload
FROM fact
ORDER BY value DESC, event_id
