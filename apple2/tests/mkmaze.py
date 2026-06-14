cols, rows = 13, 13

# 0=floor(no dot) 1=wall 2=dot 3=power
g = [[1]*cols for _ in range(rows)]

for r in range(rows):
    for c in range(cols):
        if r in (0, rows-1) or c in (0, cols-1):
            g[r][c] = 1
        else:
            ir, ic = r-1, c-1  # interior coords 0..10
            if ir % 2 == 1 and ic % 2 == 1:
                g[r][c] = 1     # pillar
            else:
                g[r][c] = 2     # dot

# ghost house: center cell, no dot
gh = (rows//2, cols//2)
g[gh[0]][gh[1]] = 0

# power pellets near the 4 inner corners
pellets = [(1,1),(1,cols-2),(rows-2,1),(rows-2,cols-2)]
for (r,c) in pellets:
    g[r][c] = 3

# flood fill from (1,1) over walkable cells (0,2,3) to confirm connectivity
from collections import deque
seen = set()
q = deque([(1,1)])
seen.add((1,1))
while q:
    r,c = q.popleft()
    for dr,dc in ((0,1),(0,-1),(1,0),(-1,0)):
        nr,nc = r+dr,c+dc
        if 0<=nr<rows and 0<=nc<cols and (nr,nc) not in seen and g[nr][nc] != 1:
            seen.add((nr,nc))
            q.append((nr,nc))

total_walkable = sum(1 for r in range(rows) for c in range(cols) if g[r][c] != 1)
print(f"cols={cols} rows={rows} walkable={total_walkable} reached={len(seen)}")
dots = sum(row.count(2) for row in g)
powers = sum(row.count(3) for row in g)
print(f"dots={dots} power={powers} total_score_dots={dots*10+powers*50}")
print(f"ghost house @ {gh}")

# ascii preview
chars = {0:' ', 1:'#', 2:'.', 3:'o'}
for r in range(rows):
    print(''.join(chars[g[r][c]] for c in range(cols)))

print()
print("pm_maze_tpl:")
for r in range(rows):
    vals = ','.join(str(g[r][c]) for c in range(cols))
    print(f"        dc.b {vals}")
