void main(in float4 Pos : SV_Position)
{
    if (Pos.z < 0.0001)
        discard;
}
