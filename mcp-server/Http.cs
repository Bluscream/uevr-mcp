using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Net;

namespace UevrMcp;

static class Http
{
    static readonly string Base = Environment.GetEnvironmentVariable("UEVR_MCP_API_URL") ?? "http://localhost:8899";
    static readonly HttpClient Client = new();
    static readonly JsonSerializerOptions JsonOptions = new()
    {
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
    };

    static string ErrorJson(string method, string url, string error, string? detail = null, int? status = null, string? body = null)
        => JsonSerializer.Serialize(new {
            error,
            method,
            url,
            detail,
            status,
            body
        }, JsonOptions);

    static async Task<string> Send(HttpRequestMessage request)
    {
        try
        {
            using var res = await Client.SendAsync(request);
            var body = await res.Content.ReadAsStringAsync();
            if (res.IsSuccessStatusCode)
            {
                return body;
            }

            return ErrorJson(
                request.Method.Method,
                request.RequestUri?.ToString() ?? request.Method.Method,
                "HTTP request failed",
                $"{(int)res.StatusCode} {res.ReasonPhrase}",
                (int)res.StatusCode,
                body);
        }
        catch (HttpRequestException ex)
        {
            return ErrorJson(
                request.Method.Method,
                request.RequestUri?.ToString() ?? request.Method.Method,
                "HTTP transport failure",
                ex.Message,
                ex.StatusCode is HttpStatusCode status ? (int)status : null);
        }
        catch (TaskCanceledException ex)
        {
            return ErrorJson(
                request.Method.Method,
                request.RequestUri?.ToString() ?? request.Method.Method,
                "HTTP request timed out",
                ex.Message);
        }
    }

    public static async Task<string> Get(string path, Dictionary<string, string?>? query = null)
    {
        var url = Base + path;
        if (query is { Count: > 0 })
        {
            var qs = string.Join("&", query
                .Where(kv => kv.Value is not null)
                .Select(kv => $"{Uri.EscapeDataString(kv.Key)}={Uri.EscapeDataString(kv.Value!)}"));
            if (qs.Length > 0) url += "?" + qs;
        }
        using var request = new HttpRequestMessage(HttpMethod.Get, url);
        return await Send(request);
    }

    public static async Task<string> Post(string path, object body)
    {
        var json = JsonSerializer.Serialize(body, JsonOptions);
        using var request = new HttpRequestMessage(HttpMethod.Post, Base + path)
        {
            Content = new StringContent(json, Encoding.UTF8, "application/json")
        };
        return await Send(request);
    }

    public static async Task<string> Delete(string path, object body)
    {
        var json = JsonSerializer.Serialize(body, JsonOptions);
        using var request = new HttpRequestMessage(HttpMethod.Delete, Base + path)
        {
            Content = new StringContent(json, Encoding.UTF8, "application/json")
        };
        return await Send(request);
    }
}
