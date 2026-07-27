SELECT event_id, drug_id, event_date, value, payload
FROM fact
WHERE value >= 0.10
ORDER BY event_id
