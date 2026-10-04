export async function fetchGatewayStatus() {
  const response = await fetch('/api/status')

  if (!response.ok) {
    throw new Error(`Gateway returned ${response.status}`)
  }

  return response.json()
}