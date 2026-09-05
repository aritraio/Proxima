// scripts/bench_k6.js -- k6 demo script (DESIGN 11.3).
// Demo tool (not throughput tool): makes graceful behavior visible.
// Run while toggling a backend to show zero failed requests and health
// checker pulling the node in/out (section 8 demo).
// Usage: k6 run scripts/bench_k6.js
import http from 'k6/http';
import { check } from 'k6';

export const options = {
  scenarios: {
    ramping: {
      executor: 'ramping-vus',
      startVUs: 1,
      stages: [
        { duration: '30s', target: 100 },
        { duration: '30s', target: 500 },
        { duration: '30s', target: 0 },
      ],
    },
  },
  thresholds: {
    http_req_failed: ['rate==0'],
    http_req_duration: ['p(95)<200'],
  },
};

export default function () {
  const res = http.get('http://127.0.0.1:8080/echo');
  check(res, {
    'status 200': (r) => r.status === 200,
    'errors == 0': (r) => r.error_code === 0,
  });
}
