param([Parameter(Mandatory)][string]$ResultsDirectory)
$ErrorActionPreference = 'Stop'
$culture = [Globalization.CultureInfo]::InvariantCulture
$metadata = Get-Content (Join-Path $ResultsDirectory 'environment.json') -Raw | ConvertFrom-Json
if (-not $metadata.completed_at -or $metadata.suite -ne 'advanced') { throw 'Complete advanced run required' }
$metadata.started_at = ([DateTimeOffset]$metadata.started_at).ToOffset([TimeSpan]::FromHours(8)).ToString('o')
$metadata.completed_at = ([DateTimeOffset]$metadata.completed_at).ToOffset([TimeSpan]::FromHours(8)).ToString('o')
$all = @(Import-Csv -LiteralPath (Join-Path $ResultsDirectory 'samples.csv'))
# 兼容历史标签，仅规范化内存中的分析记录，不改写原始 CSV。
foreach ($row in $all) {
    if ($row.implementation -eq 'paused') { $row.implementation = 'blocked_consumption' }
}
$measured = @($all | Where-Object phase -eq 'measured')
$groups = @($measured | Group-Object suite, implementation, workers, tasks, busy, iterations)
if ($groups.Count -ne 84 -or $measured.Count -ne 84 * $metadata.rounds -or $all.Count -ne 84 * ($metadata.rounds + 1)) { throw 'Missing configurations/samples' }
function Quantile([double[]]$values, [double]$q) {
    $sorted = @($values | Sort-Object)
    $index = ($sorted.Count - 1) * $q
    $lower = [int][Math]::Floor($index); $upper = [int][Math]::Ceiling($index)
    return $sorted[$lower] + ($sorted[$upper] - $sorted[$lower]) * ($index - $lower)
}
function Stats([double[]]$values) {
    if ($values.Count -lt 10) { throw 'At least 10 values required' }
    $mean = ($values | Measure-Object -Average).Average
    $squares = 0.0
    foreach ($value in $values) { $squares += ($value - $mean) * ($value - $mean) }
    return [pscustomobject]@{
        n=$values.Count; median=(Quantile $values 0.5)
        min=($values | Measure-Object -Minimum).Minimum; max=($values | Measure-Object -Maximum).Maximum
        p25=(Quantile $values 0.25); p75=(Quantile $values 0.75)
        cv_percent=([Math]::Sqrt($squares / $values.Count) / $mean * 100)
    }
}
$summaries = [Collections.Generic.List[object]]::new()
foreach ($group in $groups) {
    $first = $group.Group[0]
    $warmups = @($all | Where-Object { $_.phase -eq 'warmup' -and $_.suite -eq $first.suite -and $_.implementation -eq $first.implementation -and $_.workers -eq $first.workers -and $_.tasks -eq $first.tasks })
    $rounds = @($group.Group.round | ForEach-Object { [int]$_ } | Sort-Object -Unique)
    if ($group.Count -ne $metadata.rounds -or $rounds.Count -ne $metadata.rounds -or $rounds[0] -ne 1 -or $rounds[-1] -ne $metadata.rounds -or $warmups.Count -ne 1) { throw 'Incorrect repetitions' }
    if (@($group.Group.checksum | Sort-Object -Unique).Count -ne 1) { throw 'Checksum differs' }
    [string[]]$metrics = @('total_ms')
    if ($first.suite -eq 'backlog') {
        $metrics = @('close_ms', 'first_feedback_ms', 'all_feedback_ms', 'release_to_return_ms', 'future_cleanup_ms', 'avg_future_cleanup_us')
        foreach ($row in $group.Group) {
            if ([int]$row.broken_count -ne [int]$row.tasks -or [int]$row.executed_count -ne 0) { throw 'Discard correctness failed' }
            if ([uint64]$row.first_feedback_ns -gt [uint64]$row.all_feedback_ns -or [uint64]$row.all_feedback_ns + [uint64]$row.release_to_return_ns -ne [uint64]$row.close_ns) { throw 'Feedback timing mismatch' }
        }
    } elseif ($first.implementation -eq 'serial') { $metrics += 'serial_task_us' }
    else {
        $metrics += @('submit_ms', 'collect_ms')
        if ($first.suite -like 'enqueue_*') { $metrics += 'avg_submit_us' }
        foreach ($row in $group.Group) {
            if ([uint64]$row.submit_ns + [uint64]$row.collect_ns -ne [uint64]$row.total_ns) { throw 'Task timing mismatch' }
        }
    }
    if ($first.suite -like 'grain_*' -and [uint64]$first.tasks * [uint64]$first.iterations -ne 6553600) { throw 'Unequal total computation' }
    foreach ($metric in $metrics) {
        [double[]]$values = foreach ($row in $group.Group) {
            switch ($metric) {
                'total_ms' { [double]$row.total_ns / 1e6 }
                'submit_ms' { [double]$row.submit_ns / 1e6 }
                'collect_ms' { [double]$row.collect_ns / 1e6 }
                'serial_task_us' { [double]$row.total_ns / [double]$row.tasks / 1000 }
                'avg_submit_us' { [double]$row.submit_ns / [double]$row.tasks / 1000 }
                'close_ms' { [double]$row.close_ns / 1e6 }
                'first_feedback_ms' { [double]$row.first_feedback_ns / 1e6 }
                'all_feedback_ms' { [double]$row.all_feedback_ns / 1e6 }
                'release_to_return_ms' { [double]$row.release_to_return_ns / 1e6 }
                'future_cleanup_ms' { [double]$row.future_cleanup_ns / 1e6 }
                'avg_future_cleanup_us' { [double]$row.future_cleanup_ns / [double]$row.tasks / 1000 }
            }
        }
        $stats = Stats $values
        $summaries.Add([pscustomobject]@{
            suite=$first.suite; implementation=$first.implementation; workers=[int]$first.workers
            tasks=[int]$first.tasks; iterations=[uint64]$first.iterations; metric=$metric
            n=$stats.n; median=$stats.median; min=$stats.min; max=$stats.max; p25=$stats.p25; p75=$stats.p75
            cv_percent=$stats.cv_percent; speedup_vs_serial=$null; speedup_vs_single=$null
        })
    }
}
function Lookup([string]$suite, [string]$mode, [int]$workers, [string]$metric, [int]$tasks = 0) {
    return @($summaries | Where-Object { $_.suite -eq $suite -and $_.implementation -eq $mode -and $_.workers -eq $workers -and $_.metric -eq $metric -and ($tasks -eq 0 -or $_.tasks -eq $tasks) })[0]
}
$grains = @(32,64,128,256,512,1024,2048,4096,8192,16384,32768,65536)
foreach ($grain in $grains) {
    $suite = "grain_$grain"
    $serial = Lookup $suite serial 0 total_ms
    $single = Lookup $suite pool 1 total_ms
    foreach ($row in @($summaries | Where-Object { $_.suite -eq $suite -and $_.metric -eq 'total_ms' })) {
        $row.speedup_vs_serial = $serial.median / $row.median
        $row.speedup_vs_single = $single.median / $row.median
    }
}
foreach ($suite in @($measured.suite | Sort-Object -Unique | Where-Object { $_ -ne 'backlog' })) {
    if (@($measured | Where-Object suite -eq $suite | Select-Object -ExpandProperty checksum -Unique).Count -ne 1) { throw 'Workload mismatch' }
}
if ($summaries.Count -ne 280) { throw "Incorrect metric count: $($summaries.Count)" }
$summaries | Sort-Object suite, tasks, implementation, workers, metric | Export-Csv -LiteralPath (Join-Path $ResultsDirectory 'summary.csv') -NoTypeInformation -Encoding utf8
function Number([double]$value, [int]$digits = 3) { $value.ToString("F$digits", $culture) }
function Cell($row) { "$(Number $row.median) [$(Number $row.min), $(Number $row.max)]" }
$report = [Collections.Generic.List[string]]::new()
$report.Add('# Release 扩展性能实测')
$report.Add('')
$report.Add("时间：$($metadata.started_at) 至 $($metadata.completed_at)。CPU：$($metadata.cpu_name)，$($metadata.logical_processors) 个逻辑处理器；Release -O3 -DNDEBUG，GCC 16.2.0。")
$report.Add('')
$report.Add("84 组，各 1 次预热及 $($metadata.rounds) 次正式测量，共 $($measured.Count) 条正式记录。表格为中位数 [最小值, 最大值]；分位数/CV 见 [summary.csv](summary.csv)，各轮见 [samples.csv](samples.csv)，环境/编译命令/哈希见 [environment.json](environment.json)。不删除样本。")
$report.Add('')
$report.Add('## 固定总计算量的粒度扫描')
$report.Add('')
$report.Add('总量固定为 6,553,600 次整数递推。增加每任务迭代数、相应减少任务数；不同粒度的输入与校验值可不同，同一粒度串行与各 worker 结果完全相同。串行每任务微秒是直接串行批次时间/任务数，包含函数调用及累加；不能视为线程池任务耗时。')
$report.Add('')
$report.Add('| 每任务迭代 | 任务数 | 串行每任务 μs | 串行 ms | 1 worker ms | 2 worker ms / 对单 worker | 4 worker ms / 对单 worker | 8 worker ms / 对单 worker |')
$report.Add('| --- | --- | --- | --- | --- | --- | --- | --- |')
foreach ($grain in $grains) {
    $suite="grain_$grain"; $serial=Lookup $suite serial 0 total_ms; $single=Lookup $suite pool 1 total_ms
    $cells = foreach ($workers in @(2,4,8)) { $row=Lookup $suite pool $workers total_ms; "$(Cell $row) / $(Number $row.speedup_vs_single)x" }
    $report.Add("| $grain | $($serial.tasks) | $(Cell (Lookup $suite serial 0 serial_task_us)) | $(Cell $serial) | $(Cell $single) | $($cells -join ' | ') |")
}
$report.Add('')
foreach ($workers in @(2,4,8)) {
    $wins = @($grains | Where-Object { (Lookup "grain_$_" pool $workers total_ms).speedup_vs_single -gt 1 })
    if ($wins.Count -eq 0) { $report.Add("$workers worker 在扫描范围内未超过单 worker。"); continue }
    $first = [int]$wins[0]; $previous = @($grains | Where-Object { $_ -lt $first } | Select-Object -Last 1)
    $robust = @($grains | Where-Object { (Lookup "grain_$_" pool $workers total_ms).max -lt (Lookup "grain_$_" pool 1 total_ms).min })
    $report.Add("$workers worker 首次中位数超过单 worker：每任务 $first 次递推，串行摊销 $(Number (Lookup "grain_$first" serial 0 serial_task_us).median) μs；上一个扫描点为 $($previous -join '') 次。首次全部样本区间分离（多 worker 最大值 < 单 worker 最小值）：$($robust | Select-Object -First 1) 次。")
}
$report.Add('')
$report.Add('以上是离散扫描点的交叉区间，包含批次数量、调度、future 与实际计算；不是普适的精确阈值。原始波动完整保留。')
$report.Add('')
$report.Add('## 入队与消费分离')
$report.Add('')
$report.Add('相同极短 callable：pool 为 worker 并发消费，blocked_consumption 为每个 worker 被受控任务占用，在全部提交返回之后放行；入队期间没有基准任务执行。这是基准门控，不是产品暂停接口；历史 paused 标签按同一模式分析，原始 CSV 不变。submit 含公开接口准备、分配、锁、通知和 future 保存，不含 get；collect 含放行、消费及 get，pool 的 collect 不代表全部消费成本。受控任务、建池与容器 reserve 在计时外。')
$report.Add('')
$report.Add('| 任务数 | worker | 模式 | submit ms | μs/submit | submit 后至结果收齐 ms | 整批 ms |')
$report.Add('| --- | --- | --- | --- | --- | --- | --- |')
foreach ($tasks in @(10000,100000)) { foreach ($workers in @(1,2,4,8)) { foreach ($mode in @('pool','blocked_consumption')) {
    $suite="enqueue_$tasks"
    $report.Add("| $tasks | $workers | $mode | $(Cell (Lookup $suite $mode $workers submit_ms)) | $(Cell (Lookup $suite $mode $workers avg_submit_us)) | $(Cell (Lookup $suite $mode $workers collect_ms)) | $(Cell (Lookup $suite $mode $workers total_ms)) |")
} } }
$report.Add('')
$report.Add('## 积压 shutdown_now 与 future 清理')
$report.Add('')
$report.Add('全部 worker 被 latch 受控任务占用后，提交 1 万/10 万排队 callable。独立关闭线程计时调用 shutdown_now；主线程等待首个 future，然后逐一等待全部就绪，最后放行已领取任务。所有排队 callable 执行次数为 0，每个 get 均验证 broken_promise，已领取任务返回值正常。没有 sleep 或内部状态注入。')
$report.Add('')
$report.Add('| 积压数 | worker | 首个反馈 ms | 全部反馈观察 ms | 放行至关闭返回 ms | 关闭总 ms | get/释放 future ms | μs/异常 get |')
$report.Add('| --- | --- | --- | --- | --- | --- | --- | --- |')
foreach ($tasks in @(10000,100000)) { foreach ($workers in @(1,2,4,8)) {
    $cells=foreach ($metric in @('first_feedback_ms','all_feedback_ms','release_to_return_ms','close_ms','future_cleanup_ms','avg_future_cleanup_us')) { Cell (Lookup backlog shutdown_now $workers $metric $tasks) }
    $report.Add("| $tasks | $workers | $($cells -join ' | ') |")
} }
$report.Add('')
$report.Add('反馈时间从关闭线程调用前开始；首个为首个 future 的可观察反馈，全反馈包含观察线程的逐项 wait 开销与调度，属于丢弃反馈上界，不是内部 swap 或销毁的纯耗时。关闭总时间包含人为保持 worker 阻塞到观察完成的时间，不能解释为无门控 shutdown_now 成本。放行至返回含调度/退出/join，仍可能包含最后的丢弃收尾。future 清理单独在关闭返回后测量，含异常 throw/catch 和共享状态释放；保留 future 时用户 capture 可能仍由共享状态持有，不能将关闭返回当成所有用户对象已经销毁。各时间项的中位数不保证可相加，但每个原始样本核对全反馈+放行至返回=关闭总。')
$report.Add('')
$report.Add('单生产者，steady_clock，独立 noinline 计算 TU、禁用 IPO，每轮随机配置顺序。频率/亲和性/优先级及后台应用保持系统默认；本次合成负载与环境范围见 [PERFORMANCE.md](../../../docs/PERFORMANCE.md)。')
$report | Set-Content -LiteralPath (Join-Path $ResultsDirectory 'report.md') -Encoding utf8
$metadata | Add-Member -Force -NotePropertyName analysis_script_sha256 -NotePropertyValue (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
foreach ($artifact in @('samples.csv','summary.csv','report.md')) {
    $metadata | Add-Member -Force -NotePropertyName ($artifact.Replace('.','_')+'_sha256') -NotePropertyValue (Get-FileHash -LiteralPath (Join-Path $ResultsDirectory $artifact) -Algorithm SHA256).Hash
}
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $ResultsDirectory 'environment.json') -Encoding utf8
