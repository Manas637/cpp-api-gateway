import { useState } from 'react'

function formatChartTime(timestamp) {
  return new Date(timestamp).toLocaleTimeString([], {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  })
}

function LatencyChart({ data }) {
  const width = 800
  const height = 320

  // EXACTLY the same geometry as RequestRateChart.
  const padding = {
    top: 20,
    right: 20,
    bottom: 60,
    left: 55,
  }

  const [hoveredIndex, setHoveredIndex] = useState(null)

  if (data.length < 2) {
    return (
      <div className="chart-empty">
        Collecting latency data...
      </div>
    )
  }

  const chartWidth =
    width - padding.left - padding.right

  const chartHeight =
    height - padding.top - padding.bottom

  /*
   * --------------------------------------------------
   * Time scale
   * --------------------------------------------------
   */

  const startTime = new Date(
    data[0].timestamp
  ).getTime()

  const endTime = new Date(
    data[data.length - 1].timestamp
  ).getTime()

  const timeRange = Math.max(
    endTime - startTime,
    1
  )

  const getX = (index) => {
    const timestamp = new Date(
      data[index].timestamp
    ).getTime()

    const ratio =
      (timestamp - startTime) / timeRange

    return padding.left + ratio * chartWidth
  }

  /*
   * --------------------------------------------------
   * Y scale
   * --------------------------------------------------
   */

  const maxValue = Math.max(
    ...data.flatMap((point) => [
      point.p50,
      point.p95,
      point.p99,
    ]),
    0.001
  )

  const maxMilliseconds =
    maxValue * 1000

  const yMax =
    Math.ceil(
      maxMilliseconds / 10
    ) * 10 || 10

  const getY = (seconds) => {
    const milliseconds =
      seconds * 1000

    return (
      padding.top +
      chartHeight -
      (milliseconds / yMax) *
        chartHeight
    )
  }

  /*
   * --------------------------------------------------
   * Lines
   * --------------------------------------------------
   */

  const createPoints = (key) =>
    data
      .map(
        (point, index) =>
          `${getX(index)},${getY(point[key])}`
      )
      .join(' ')

  /*
   * --------------------------------------------------
   * Y-axis
   * --------------------------------------------------
   */

  const gridLines = 4

  const yTicks = Array.from(
    { length: gridLines + 1 },
    (_, index) =>
      (yMax / gridLines) * index
  )

  /*
   * --------------------------------------------------
   * X-axis timestamps
   * --------------------------------------------------
   */

  const middleTimestamp =
    startTime + timeRange / 2

  const middleIndex = data.reduce(
    (closestIndex, point, index) => {
      const pointTime = new Date(
        point.timestamp
      ).getTime()

      const closestTime = new Date(
        data[closestIndex].timestamp
      ).getTime()

      return Math.abs(
        pointTime - middleTimestamp
      ) <
        Math.abs(
          closestTime - middleTimestamp
        )
        ? index
        : closestIndex
    },
    0
  )

  const timeIndexes = [
    0,
    middleIndex,
    data.length - 1,
  ]

  /*
   * --------------------------------------------------
   * Hover
   * --------------------------------------------------
   */

  const handleMouseMove = (event) => {
    const rect =
      event.currentTarget.getBoundingClientRect()

    const mouseX =
      ((event.clientX - rect.left) / rect.width) *
      width

    const clampedX = Math.max(
      padding.left,
      Math.min(
        width - padding.right,
        mouseX
      )
    )

    const ratio =
      (clampedX - padding.left) /
      chartWidth

    const targetTimestamp =
      startTime + ratio * timeRange

    let closestIndex = 0
    let closestDistance = Infinity

    data.forEach((point, index) => {
      const timestamp = new Date(
        point.timestamp
      ).getTime()

      const distance = Math.abs(
        timestamp - targetTimestamp
      )

      if (distance < closestDistance) {
        closestDistance = distance
        closestIndex = index
      }
    })

    setHoveredIndex(closestIndex)
  }

  const handleMouseLeave = () => {
    setHoveredIndex(null)
  }

  const hoveredPoint =
    hoveredIndex !== null
      ? data[hoveredIndex]
      : null

  /*
   * --------------------------------------------------
   * Tooltip positioning
   * --------------------------------------------------
   */

  let tooltipX = 0
  let tooltipY = 0

  if (hoveredPoint) {
    const pointX = getX(hoveredIndex)
    const pointY = getY(hoveredPoint.p99)

    tooltipX =
      pointX > width / 2
        ? pointX - 177
        : pointX + 12

    tooltipY = Math.max(
      padding.top + 8,
      pointY - 55
    )
  }

  return (
    <div className="chart-container">
      <svg
        viewBox={`0 0 ${width} ${height}`}
        preserveAspectRatio="none"
        className="latency-chart"
        onMouseMove={handleMouseMove}
        onMouseLeave={handleMouseLeave}
      >
        {/* Y-axis grid */}
        {yTicks.map((value) => {
          const y =
            padding.top +
            chartHeight -
            (value / yMax) *
              chartHeight

          return (
            <g key={value}>
              <line
                x1={padding.left}
                y1={y}
                x2={width - padding.right}
                y2={y}
                className="chart-grid-line"
              />

              <text
                x={padding.left - 10}
                y={y + 4}
                textAnchor="end"
                className="chart-axis-label"
              >
                {Math.round(value)}
              </text>
            </g>
          )
        })}

        {/* P50 */}
        <polyline
          points={createPoints('p50')}
          fill="none"
          stroke="currentColor"
          strokeWidth="2"
          strokeLinejoin="round"
          strokeLinecap="round"
          className="latency-p50"
        />

        {/* P95 */}
        <polyline
          points={createPoints('p95')}
          fill="none"
          stroke="currentColor"
          strokeWidth="2"
          strokeLinejoin="round"
          strokeLinecap="round"
          className="latency-p95"
        />

        {/* P99 */}
        <polyline
          points={createPoints('p99')}
          fill="none"
          stroke="currentColor"
          strokeWidth="2"
          strokeLinejoin="round"
          strokeLinecap="round"
          className="latency-p99"
        />

        {/* Hover state */}
        {hoveredPoint && (
          <>
            <line
              x1={getX(hoveredIndex)}
              y1={padding.top}
              x2={getX(hoveredIndex)}
              y2={padding.top + chartHeight}
              className="chart-hover-line"
            />

            <circle
              cx={getX(hoveredIndex)}
              cy={getY(hoveredPoint.p50)}
              r="5"
              className="hover-point p50-point"
            />

            <circle
              cx={getX(hoveredIndex)}
              cy={getY(hoveredPoint.p95)}
              r="5"
              className="hover-point p95-point"
            />

            <circle
              cx={getX(hoveredIndex)}
              cy={getY(hoveredPoint.p99)}
              r="5"
              className="hover-point p99-point"
            />

            <g
              transform={`translate(
                ${tooltipX},
                ${tooltipY}
              )`}
            >
              <rect
                width="165"
                height="105"
                rx="8"
                className="chart-tooltip"
              />

              <text
                x="12"
                y="20"
                className="tooltip-time"
              >
                {formatChartTime(
                  hoveredPoint.timestamp
                )}
              </text>

              <text
                x="12"
                y="43"
                className="tooltip-p50"
              >
                P50:{' '}
                {(
                  hoveredPoint.p50 * 1000
                ).toFixed(2)}{' '}
                ms
              </text>

              <text
                x="12"
                y="65"
                className="tooltip-p95"
              >
                P95:{' '}
                {(
                  hoveredPoint.p95 * 1000
                ).toFixed(2)}{' '}
                ms
              </text>

              <text
                x="12"
                y="87"
                className="tooltip-p99"
              >
                P99:{' '}
                {(
                  hoveredPoint.p99 * 1000
                ).toFixed(2)}{' '}
                ms
              </text>
            </g>
          </>
        )}

        {/* X-axis timestamps */}
        {timeIndexes.map((index) => {
          const point = data[index]

          return (
            <text
              key={point.timestamp}
              x={getX(index)}
              y={height - 24}
              textAnchor="middle"
              className="chart-axis-label chart-time-label"
            >
              {formatChartTime(point.timestamp)}
            </text>
          )
        })}
      </svg>
    </div>
  )
}

export default LatencyChart