async function queryPrometheus(query) {
  const params = new URLSearchParams({ query })

  const response = await fetch(
    `/prometheus/api/v1/query?${params.toString()}`
  )

  if (!response.ok) {
    throw new Error(`Prometheus returned ${response.status}`)
  }

  const data = await response.json()

  if (data.status !== 'success') {
    throw new Error('Prometheus query failed')
  }

  return data.data.result
}

export async function fetchRequestRate() {
  const result = await queryPrometheus(
    'rate(gateway_requests_total[1m])'
  )

  return result.length > 0
    ? Number(result[0].value[1])
    : 0
}

export async function fetchLatencyPercentiles() {
  const queries = {
    p50: `
      histogram_quantile(
        0.50,
        sum(rate(gateway_request_duration_seconds_bucket[1m])) by (le)
      )
    `,
    p95: `
      histogram_quantile(
        0.95,
        sum(rate(gateway_request_duration_seconds_bucket[1m])) by (le)
      )
    `,
    p99: `
      histogram_quantile(
        0.99,
        sum(rate(gateway_request_duration_seconds_bucket[1m])) by (le)
      )
    `,
  }

  const entries = await Promise.all(
    Object.entries(queries).map(async ([percentile, query]) => {
      const result = await queryPrometheus(query)

      const value =
        result.length > 0
          ? Number(result[0].value[1])
          : 0

      return [
        percentile,
        Number.isFinite(value) ? value : 0,
      ]
    })
  )

  return Object.fromEntries(entries)
}