**n = 1,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | ankerl | absl_flat | absl_node | dense_hash | sparse_hash | std |
|---|---|---|---|---|---|---|---|---|---|---|
| insert (reserved) | **23.5** | 38.7 | 38.6 | 124.1 | 35.4 | 44.5 | 76.2 | 39.0 | 208.6 | 141.7 |
| find hit (insert order) | 13.0 | **12.2** | 15.6 | 25.3 | 14.4 | 15.3 | 18.7 | 16.9 | 67.7 | 33.3 |
| find hit (random order) | **15.3** | 17.3 | 15.7 | 29.6 | 23.5 | 16.4 | 27.0 | 17.0 | 75.9 | 38.3 |
| find miss | **8.2** | 13.0 | 8.8 | 25.4 | 10.4 | 11.9 | 13.9 | 24.3 | 49.7 | 52.9 |
| erase | 27.3 | 28.9 | 19.3 | 51.8 | 30.8 | 28.1 | 48.1 | **19.0** | 130.0 | 112.8 |
| iterate | 3.1 | 0.7 | 11.5 | 17.2 | **0.6** | 10.2 | 18.1 | 23.1 | 20.8 | 45.0 |
| insert (no reserve) | 69.8 | 107.4 | 68.4 | 201.5 | 60.8 | **59.5** | 125.5 | 69.9 | 341.5 | 279.2 |
| bytes/entry (steady) | 21.8 | 34.2 | 34.9 | 46.9 | 29.0 | 37.0 | 52.5 | 34.9 | **19.8** | 42.1 |
| bytes/entry (growth peak) | 40.9 | 36.2 | 51.5 | 46.9 | 31.9 | 54.5 | 58.6 | 51.3 | **20.0** | 45.4 |

Run spread (min-max, ns/op) of sflat and ankerl:

- sflat: insert (reserved) 19.3-40.0, find hit (random order) 11.7-16.6, find miss 7.1-9.8, no reserve 64.8-74.9
- ankerl: insert (reserved) 35.4-39.9, find hit (random order) 23.3-24.7, find miss 10.3-10.8, no reserve 56.0-109.1

**n = 10,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | ankerl | absl_flat | absl_node | dense_hash | sparse_hash | std |
|---|---|---|---|---|---|---|---|---|---|---|
| insert (reserved) | **33.4** | 67.6 | 68.4 | 222.0 | 71.0 | 65.6 | 134.6 | 60.8 | 445.0 | 302.0 |
| find hit (insert order) | 45.1 | 32.1 | 37.4 | 52.8 | **26.6** | 33.0 | 30.2 | 36.5 | 161.0 | 67.0 |
| find hit (random order) | 34.7 | 42.6 | 35.5 | 71.5 | 66.3 | 34.3 | 53.2 | **31.0** | 164.9 | 90.9 |
| find miss | 26.7 | 25.3 | **17.0** | 60.7 | 21.7 | 22.6 | 26.6 | 35.9 | 131.1 | 96.6 |
| erase | 57.9 | 63.0 | **36.4** | 151.9 | 68.4 | 59.8 | 103.3 | 58.0 | 214.4 | 213.6 |
| iterate | 5.6 | 1.2 | 11.8 | 42.5 | **1.1** | 9.1 | 24.1 | 35.4 | 31.9 | 85.1 |
| insert (no reserve) | **77.9** | 131.7 | 80.5 | 314.7 | 89.4 | 99.8 | 201.6 | 157.0 | 611.1 | 502.2 |
| bytes/entry (steady) | 20.0 | 29.5 | 27.0 | 42.8 | 25.4 | 28.7 | 47.2 | 53.8 | **18.1** | 40.4 |
| bytes/entry (growth peak) | 30.1 | 29.6 | 40.4 | 42.8 | 36.2 | 42.9 | 47.2 | 80.6 | **18.1** | 41.9 |

Run spread (min-max, ns/op) of sflat and ankerl:

- sflat: insert (reserved) 32.8-43.1, find hit (random order) 30.9-41.6, find miss 22.3-28.4, no reserve 64.7-105.4
- ankerl: insert (reserved) 66.0-74.5, find hit (random order) 52.9-70.5, find miss 20.3-33.6, no reserve 86.4-92.4

**n = 100,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | ankerl | absl_flat | absl_node | dense_hash | sparse_hash | std |
|---|---|---|---|---|---|---|---|---|---|---|
| insert (reserved) | **65.8** | 134.8 | 107.8 | 361.1 | 113.8 | 115.0 | 253.9 | 85.1 | 730.7 | 468.9 |
| find hit (insert order) | 43.3 | **36.2** | 57.8 | 70.0 | 51.2 | 53.7 | 56.2 | 42.8 | 251.9 | 87.1 |
| find hit (random order) | 51.7 | 60.8 | 59.4 | 93.4 | 79.4 | 55.7 | 85.3 | **44.6** | 240.0 | 114.1 |
| find miss | **35.2** | 39.5 | 38.5 | 92.0 | 43.6 | 53.7 | 63.6 | 54.5 | 315.7 | 124.8 |
| erase | 94.6 | 92.7 | 64.7 | 189.0 | 112.3 | 97.9 | 156.8 | **51.4** | 310.0 | 329.3 |
| iterate | 5.4 | **1.2** | 11.8 | 58.6 | **1.2** | 10.7 | 29.7 | 30.4 | 30.6 | 124.2 |
| insert (no reserve) | **94.0** | 210.2 | 166.2 | 474.5 | 166.1 | 148.2 | 328.6 | 227.4 | 927.1 | 782.5 |
| bytes/entry (steady) | 19.6 | 26.8 | 21.5 | 40.6 | 23.4 | 22.8 | 44.1 | 43.0 | **17.8** | 40.1 |
| bytes/entry (growth peak) | 23.8 | 26.8 | 32.2 | 40.6 | 28.9 | 34.2 | 44.1 | 64.4 | **17.8** | 40.1 |

Run spread (min-max, ns/op) of sflat and ankerl:

- sflat: insert (reserved) 54.5-77.5, find hit (random order) 48.9-54.0, find miss 29.9-37.7, no reserve 83.3-102.1
- ankerl: insert (reserved) 108.3-122.1, find hit (random order) 76.5-84.0, find miss 43.4-53.1, no reserve 145.1-178.3

