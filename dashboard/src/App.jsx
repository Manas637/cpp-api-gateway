import { useEffect, useState } from 'react'
import { fetchGatewayStatus } from './services/api'
import {
  fetchRequestRate,
  fetchLatencyPercentiles,
} from './services/prometheus'

import RequestRateChart from './components/RequestRateChart'
import LatencyChart from './components/LatencyChart'

function App() {
  const [status, setStatus] = useState(null)
  const [requestRate, setRequestRate] = useState(0)
  const [latency, setLatency] = useState({
    p50: 0,
    p95: 0,
    p99: 0,
  })
  const [requestRateHistory, setRequestRateHistory] = useState([])
  const [latencyHistory, setLatencyHistory] = useState([])
  const [error, setError] = useState(null)

  useEffect(() => {
    let cancelled = false

    const loadDashboardData = async () => {
      try {
        const [gatewayStatus, rate, latencyPercentiles] =
          await Promise.all([
            fetchGatewayStatus(),
            fetchRequestRate(),
            fetchLatencyPercentiles(),
          ])

        if (!cancelled) {
          setStatus(gatewayStatus)
          setRequestRate(rate)
          setLatency(latencyPercentiles)
          setError(null)

          setRequestRateHistory((previous) => [
            ...previous.slice(-59),
            {
              timestamp: Date.now(),
              value: rate,
            },
          ])
          setLatencyHistory((previous) => [
            ...previous.slice(-59),
            {
              timestamp: Date.now(),
              p50: latencyPercentiles.p50,
              p95: latencyPercentiles.p95,
              p99: latencyPercentiles.p99,
            },
          ])
        }
      } catch (err) {
        if (!cancelled) {
          setError(err.message)
        }
      }
    }

    loadDashboardData()

    const intervalId = setInterval(
      loadDashboardData,
      3000
    )

    return () => {
      cancelled = true
      clearInterval(intervalId)
    }
  }, [])

  if (error) {
    return (
      <main className="dashboard">
        <div className="error-card">
          <h2>Gateway Unavailable</h2>
          <p>{error}</p>
        </div>
      </main>
    )
  }

  if (!status) {
    return (
      <main className="dashboard">
        <div className="loading">
          Loading gateway status...
        </div>
      </main>
    )
  }

  const healthyBackends = status.backends.filter(
    (backend) =>
      backend.healthy && backend.circuit !== 'OPEN'
  ).length

  return (
    <main className="dashboard">
      <header className="dashboard-header">
        <div>
          <p className="eyebrow">OPERATIONS</p>

          <h1>C++ API Gateway</h1>

          <p className="subtitle">
            Real-time gateway health and backend status
          </p>
        </div>

        <div className={`status-badge ${status.status}`}>
          <span className="status-dot" />
          {status.status.toUpperCase()}
        </div>
      </header>

      <section className="summary-grid">
        <div className="summary-card">
          <span className="card-label">
            Active Connections
          </span>

          <strong>
            {status.active_connections}
          </strong>
        </div>

        <div className="summary-card">
          <span className="card-label">
            Healthy Backends
          </span>

          <strong>
            {healthyBackends}/{status.backends.length}
          </strong>
        </div>

        <div className="summary-card">
          <span className="card-label">
            Rate Limiting
          </span>

          <strong>
            {status.rate_limiting.enabled
              ? 'ON'
              : 'OFF'}
          </strong>
        </div>
      </section>

      <section className="section">
        <div className="section-header">
          <div>
            <h2>Backend Health</h2>

            <p>
              Current state of configured upstream
              servers
            </p>
          </div>
        </div>

        <div className="backend-grid">
          {status.backends.map((backend) => {
            const isHealthy =
              backend.healthy &&
              backend.circuit !== 'OPEN'

            return (
              <article
                className={`backend-card ${
                  isHealthy
                    ? 'healthy'
                    : 'unhealthy'
                }`}
                key={`${backend.host}:${backend.port}`}
              >
                <div className="backend-header">
                  <div>
                    <span className="backend-label">
                      BACKEND
                    </span>

                    <h3>
                      {backend.host}:{backend.port}
                    </h3>
                  </div>

                  <span
                    className={`health-pill ${
                      isHealthy
                        ? 'healthy'
                        : 'unhealthy'
                    }`}
                  >
                    {isHealthy
                      ? 'Healthy'
                      : 'Unhealthy'}
                  </span>
                </div>

                <div className="backend-details">
                  <div>
                    <span>Circuit</span>
                    <strong>
                      {backend.circuit}
                    </strong>
                  </div>

                  <div>
                    <span>Failures</span>
                    <strong>
                      {backend.failure_count}
                    </strong>
                  </div>
                </div>
              </article>
            )
          })}
        </div>
      </section>

      <section className="section">
        <div className="section-header">
          <div>
            <h2>Traffic & Latency</h2>
            <p>Live metrics from Prometheus</p>
          </div>
        </div>

        <div className="metrics-grid">
          <div className="metric-card">
            <span className="card-label">Request Rate</span>

            <strong>
              {requestRate.toFixed(2)}
            </strong>

            <span className="metric-unit">
              req/s
            </span>
          </div>

          <div className="metric-card">
            <span className="card-label">P50 Latency</span>

            <strong>
              {(latency.p50 * 1000).toFixed(2)}
            </strong>

            <span className="metric-unit">
              ms
            </span>
          </div>

          <div className="metric-card">
            <span className="card-label">P95 Latency</span>

            <strong>
              {(latency.p95 * 1000).toFixed(2)}
            </strong>

            <span className="metric-unit">
              ms
            </span>
          </div>

          <div className="metric-card">
            <span className="card-label">P99 Latency</span>

            <strong>
              {(latency.p99 * 1000).toFixed(2)}
            </strong>

            <span className="metric-unit">
              ms
            </span>
          </div>
        </div>

        <div className="chart-card">
          <div className="chart-header">
            <div>
              <h3>Request Rate</h3>
              <p>Rolling 3-minute window</p>
            </div>

            <strong>
              {requestRate.toFixed(2)} req/s
            </strong>
          </div>

          <RequestRateChart data={requestRateHistory} />
        </div>
      </section>
      <div className="chart-card">
        <div className="chart-header">
          <div>
            <h3>Latency Percentiles</h3>
            <p>Rolling 3-minute window</p>
          </div>

          <div className="latency-legend">
            <span className="legend-p50">P50</span>
            <span className="legend-p95">P95</span>
            <span className="legend-p99">P99</span>
          </div>
        </div>

        <LatencyChart data={latencyHistory} />
      </div>
    </main>
  )
}

export default App